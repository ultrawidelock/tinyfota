#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Aseem Chopra
# SPDX-License-Identifier: Apache-2.0

"""Prove `tinyfota push --native` and the scan, without a radio.

    python tests/tooling/native_check.py      # with tinyfota installed

The native client (tinyfota/smp.py: Native, push_native) talks to a fake
board that answers the receiver's frames the way core/dfu_receiver.c does:
BEGIN claims, DATA at the receiver's offset appends and DATA anywhere else is
answered with that offset, COMMIT checks the length. The fake drops frames and
replies on purpose, which is what the resend logic is for. The scan
(Sightings) is fed advertisement and scan-response reports the way bleak's
detection callback delivers them. Last, run() goes through once end to end
with bleak itself replaced by doubles of its scanner and client.
"""

import asyncio
import struct
import sys
from types import SimpleNamespace

from tinyfota import smp

fails = 0
total = 0


def check(name, ok, detail=""):
    global fails, total
    total += 1
    print(f"  {'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f"  {detail}"))
    if not ok:
        fails += 1


class FakeBoard:
    """The receiver's native frames, as core/dfu_receiver.c answers them."""

    def __init__(self, window=True):
        self.window = window
        self.tid = 0
        self.total = 0
        self.data = bytearray()
        self.staged = False

    def frame(self, f):
        ok = lambda: struct.pack("<BII", smp.NATIVE_OK, self.tid, len(self.data))  # noqa: E731
        err = lambda e: bytes([smp.NATIVE_ERR, e])  # noqa: E731
        if not self.window:
            return err(1)
        op = f[0]
        if op == smp.NATIVE_BEGIN and len(f) == 9:
            tid, tot = struct.unpack_from("<II", f, 1)
            if not (tid == self.tid and tot == self.total):
                self.tid, self.total, self.data = tid, tot, bytearray()
            return ok()
        if op == smp.NATIVE_DATA and len(f) >= 9:
            tid, off = struct.unpack_from("<II", f, 1)
            if tid != self.tid:
                return err(2)
            if off == len(self.data):
                if off + len(f) - 9 > self.total:
                    return err(3)
                self.data += f[9:]
            return ok()
        if op == smp.NATIVE_COMMIT and len(f) == 5:
            if len(self.data) != self.total:
                return err(2)
            self.staged = True
            return ok()
        return err(7)


class FakeClient:
    """A BleakClient's write_gatt_char, delivering the board's reply as a
    notification. drop_frame / drop_reply pick writes to lose by count."""

    def __init__(self, board, drop_frame=(), drop_reply=(), mtu=247):
        self.board = board
        self.native = None
        self.drop_frame = set(drop_frame)
        self.drop_reply = set(drop_reply)
        self.writes = []
        self.mtu = mtu

    async def write_gatt_char(self, uuid, data, response=False):
        n = len(self.writes)
        self.writes.append((uuid, bytes(data), response))
        if len(data) > self.mtu - 3:
            raise ValueError("write longer than the ATT MTU allows")
        if n in self.drop_frame:
            return
        rsp = self.board.frame(bytes(data))
        if n not in self.drop_reply:
            self.native.on_notify(None, rsp)


def push(blob, mtu=247, **kw):
    board = FakeBoard(window=kw.pop("window", True))
    client = FakeClient(board, mtu=mtu, **kw)
    native = smp.Native(client, mtu)
    native.timeout = 0.02
    client.native = native
    try:
        frames = asyncio.run(smp.push_native(native, blob, transfer_id=0x51))
        return board, client, frames, None
    except SystemExit as exc:
        return board, client, None, str(exc)


def run_quiet(fn, *a, **kw):
    """Run with the client's progress lines kept off the report."""
    import contextlib
    import io

    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*a, **kw)


def main():
    blob = b"WDFU" + bytes((i * 7) & 0xFF for i in range(2000))

    print("native push")
    board, client, frames, err = run_quiet(push, blob)
    check("clean push stages the file", err is None and board.staged and bytes(board.data) == blob,
          err or "")
    check("DATA frames fill the 247 B MTU", max(len(w[1]) for w in client.writes) == 244)
    check("frames: BEGIN, ceil(2004 / 235) DATA, COMMIT", frames == 1 + 9 + 1, str(frames))
    check("every write goes to the native characteristic",
          all(w[0] == smp.NATIVE_CHR_UUID for w in client.writes))

    board, client, frames, err = run_quiet(push, blob, mtu=23)
    check("MTU 23: 11 B of data a frame", err is None and board.staged
          and max(len(w[1]) for w in client.writes) == 20, err or "")

    board, client, frames, err = run_quiet(push, blob, drop_reply={0, 3, 4, 9})
    check("lost replies are resent and the transfer still lands",
          err is None and board.staged and bytes(board.data) == blob, err or "")

    board, client, frames, err = run_quiet(push, blob, drop_frame={2, 5, 11})
    check("lost frames are resent from the board's offset",
          err is None and board.staged and bytes(board.data) == blob, err or "")

    board, client, frames, err = run_quiet(push, blob, window=False)
    check("closed window ends with the board's reason", err is not None
          and "no update window" in err, str(err))

    board, client, frames, err = run_quiet(push, blob, drop_reply=set(range(1, 4)))
    check("a board that never answers DATA ends, it does not hang",
          err is not None and "stopped answering" in err, str(err))

    print("native payload")
    hdr = struct.pack("<IIHHII", 0x96F3B83D, 0, 0x20, 0, len(blob), 0).ljust(0x20, b"\0")
    check("a raw .wdfu goes as it is", smp.native_payload(blob) == blob)
    check("a wrapped one is unwrapped", smp.native_payload(hdr + blob + b"TLV") == blob)
    whole = struct.pack("<IIHHII", 0x96F3B83D, 0, 0x20, 0, 64, 0).ljust(0x20, b"\0") + bytes(64)
    try:
        smp.native_payload(whole)
        check("a whole image is refused", False)
    except SystemExit as exc:
        check("a whole image is refused", "serial recovery" in str(exc), str(exc))

    print("scan")
    seen = smp.Sightings((smp.SMP_SVC_SHORT,))
    dev = SimpleNamespace(address="AA", name="tinytalk")
    other = SimpleNamespace(address="BB", name="other")
    adv = SimpleNamespace(service_uuids=["0000fff6-0000-1000-8000-00805f9b34fb"], service_data={})
    rsp = SimpleNamespace(service_uuids=[smp.SMP_SVC_UUID.upper()], service_data={})
    seen.see(dev, adv)
    seen.see(dev, rsp)
    seen.see(dev, adv)          # the advertisement again, after the scan response
    seen.see(other, adv)
    check("UUID from a scan response survives a later advertisement",
          [d.address for d in seen.boards()] == ["AA"])
    check("--address filters", seen.boards("aa") == [dev] and seen.boards("cc") == [])
    data = smp.Sightings((smp.NATIVE_SVC_SHORT, smp.SMP_SVC_SHORT))
    data.see(other, SimpleNamespace(service_uuids=[], service_data={smp.NATIVE_SVC_UUID: b""}))
    check("service data counts, and --native takes either service",
          [d.address for d in data.boards()] == ["BB"])

    print("dry run, bleak faked")
    fake_board = FakeBoard()
    reports = [
        (SimpleNamespace(address="AA", name="tinytalk"),
         SimpleNamespace(service_uuids=[smp.SMP_SVC_UUID], service_data={})),
        (SimpleNamespace(address="AA", name="tinytalk"),
         SimpleNamespace(service_uuids=[], service_data={})),
    ]

    class Scanner:
        def __init__(self, detection_callback):
            self.cb = detection_callback

        async def __aenter__(self):
            for dev, adv in reports:
                self.cb(dev, adv)
            return self

        async def __aexit__(self, *exc):
            return False

    class Client(FakeClient):
        def __init__(self, device, services):
            super().__init__(fake_board, mtu=185)
            self.mtu_size = 185
            self.services = services
            opened.append(self)

        async def __aenter__(self):
            return self

        async def __aexit__(self, *exc):
            return False

        async def start_notify(self, uuid, cb):
            self.notify_uuid = uuid
            self.native = SimpleNamespace(on_notify=cb)

    opened = []
    sys.modules["bleak"] = SimpleNamespace(BleakScanner=Scanner, BleakClient=Client)
    import tempfile

    with tempfile.TemporaryDirectory() as d:
        path = f"{d}/u.wdfu"
        with open(path, "wb") as fh:
            fh.write(blob)
        args = SimpleNamespace(serial="", native=True, patch=path, scan=0.01, address="",
                               list=False, expect="", no_reset=False)
        orig_sleep = asyncio.sleep
        try:
            smp.asyncio.sleep = lambda t: orig_sleep(min(t, 0.01))
            run_quiet(asyncio.run, smp.run(args))
        finally:
            smp.asyncio.sleep = orig_sleep
    c = opened[0] if opened else None
    check("found by the SMP UUID in a report that was not the last", c is not None)
    check("discovery limited to the native service",
          c is not None and c.services == [smp.NATIVE_SVC_UUID])
    check("notifications on the native characteristic",
          c is not None and c.notify_uuid == smp.NATIVE_CHR_UUID)
    check("the board ends up staged, frames sized to MTU 185",
          fake_board.staged and bytes(fake_board.data) == blob
          and max(len(w[1]) for w in c.writes) == 182)
    del sys.modules["bleak"]

    print("arguments")
    for extra in (["--list"], ["--serial", "x"], ["--no-reset"]):
        try:
            smp.push(SimpleNamespace(patch="u.wdfu", list="--list" in extra, expect="",
                                     serial="x" if "--serial" in extra else "",
                                     no_reset="--no-reset" in extra, native=True))
            check(f"--native refuses {extra[0]}", False)
        except SystemExit as exc:
            check(f"--native refuses {extra[0]}", "--native" in str(exc), str(exc))

    if fails:
        print(f"\n  {fails} of {total} checks FAILED")
        sys.exit(1)
    print(f"\nRESULT: PASS ({total} checks)")


if __name__ == "__main__":
    main()
