# SPDX-FileCopyrightText: 2026 Aseem Chopra
# SPDX-License-Identifier: Apache-2.0

"""Push a delta patch to the board over SMP, the way a phone would.

This speaks mcumgr over GATT -- byte for byte what nRF Device Manager sends --
or over a serial cable, so the device half can be proved from a computer
before anyone starts tapping at a phone. When this works and the app does not,
the fault is in the app or the file it is given, not in the firmware.

    tinyfota push update.wdfu                 push a patch
    tinyfota push --list                      read the image list and stop
    tinyfota push --native update.wdfu        push over tinyfota's own frames

Requires the board to serve the SMP image group (CONFIG_TINYFOTA_SMP_IMG), or,
with --native, the native GATT service (CONFIG_TINYFOTA_TINYBLE or
CONFIG_TINYFOTA_BLE_GATT).

CBOR IS HAND-ROLLED HERE, deliberately. The maps mcumgr exchanges are half a
dozen keys of ints and byte strings, and vendoring a CBOR library into the OTA
venv to encode that would be more moving parts than the encoder itself.
"""

import argparse
import base64
import binascii
import asyncio
import os
import struct
import sys
from pathlib import Path

# Zephyr's SMP service, subsys/mgmt/mcumgr/transport/src/smp_bt.c.
SMP_SVC_UUID = "8d53dc1d-1db7-4cd3-868b-8a527460aa84"
SMP_CHR_UUID = "da2e7828-fbce-4e01-ae9e-261174997c48"

# HOW A BOARD IS RECOGNISED: by the SMP service UUID, and by nothing else.
#
# The source project learned this the hard way (measured there on 2026-08-27,
# a board at -53 dBm). Its name filter missed a board in plain view because an
# advertisement is 31 bytes and the controller SHORTENS the local name when it
# does not fit -- 'ultrawidelo' for the name it was looking for -- and the miss
# was intermittent, since a scan response or the OS cache sometimes supplied
# the full name. The SMP service UUID was in the advertisement all along, and it
# is the one identifier meaning "this board speaks exactly the protocol I am
# about to speak". It is checked in the UUID list and in the service data.
SMP_SVC_SHORT = "8d53dc1d"

# The receiver's own frames (FORMAT.md, core/tinyfota_rx.h) on one notifying
# characteristic. A board on tinyble serves it beside SMP and advertises only
# the SMP UUID, so --native finds a board by either.
NATIVE_SVC_UUID = "d3b5a140-9e23-4b3a-8be4-6b1ee5f980a3"
NATIVE_CHR_UUID = "d3b5a141-9e23-4b3a-8be4-6b1ee5f980a3"
NATIVE_SVC_SHORT = "d3b5a140"
NATIVE_BEGIN, NATIVE_DATA, NATIVE_COMMIT = 0x11, 0x12, 0x13
NATIVE_OK, NATIVE_ERR = 0x81, 0x82
NATIVE_ERRORS = {
    1: "no update window is open (press the board's update button)",
    2: "out of sequence",
    3: "too large for patch_staging",
    4: "the header signature did not verify",
    5: "length or CRC disagreed at commit",
    6: "a flash write or erase failed",
    7: "malformed frame",
    8: "another transport owns the receiver",
    9: "built for a different image than the one running",
}

OP_READ_REQ, OP_READ_RSP, OP_WRITE_REQ, OP_WRITE_RSP = 0, 1, 2, 3
GRP_OS, GRP_IMG = 0, 1
OS_ID_RESET = 5
IMG_ID_STATE, IMG_ID_UPLOAD = 0, 1

# mgmt_defines.h. Only the ones this tool can actually provoke are named.
MGMT_ERR = {
    0: "ok",
    2: "out of memory",
    3: "malformed request",
    6: "bad state",
    8: "not supported",
    11: "access denied -- no update window is open (press the board's update button)",
}


def die(msg):
    """Exit the process with the formatted error message prefixed by tinyfota."""
    sys.exit(f"tinyfota: {msg}")


def image_sha(path):
    """The SHA-256 MCUboot recorded in a signed image's TLVs.

    The same hash the board reports in its image list, so comparing the two
    answers "did the update actually land" with the image's own bytes rather
    than with anyone's assumption. Used after a phone push, where nothing else
    on this machine witnessed the transfer.
    """
    d = Path(path).read_bytes()
    magic, _, hdr_sz, _, img_sz, _ = struct.unpack_from("<IIHHII", d, 0)
    if magic != 0x96F3B83D:
        die(f"{path} is not an MCUboot image")

    base = hdr_sz + img_sz
    tlv_magic, tlv_tot = struct.unpack_from("<HH", d, base)
    if tlv_magic == 0x6908:  # protected block first, unprotected after it
        base += tlv_tot
        tlv_magic, tlv_tot = struct.unpack_from("<HH", d, base)
    if tlv_magic != 0x6907:
        die(f"{path} has no TLV block where its header says it should")

    p, end = base + 4, base + tlv_tot
    while p + 4 <= end:
        kind, _, ln = struct.unpack_from("<BBH", d, p)
        p += 4
        if kind == 0x10 and ln == 32:
            return d[p : p + 32]
        p += ln
    die(f"{path} has no SHA-256 TLV")


# ---- the smallest CBOR that carries an mcumgr request ------------------------


def _head(major, n):
    """Encode a CBOR unsigned integer length prefix for the given value n and major type. Returns 1, 2, 3, or 5 bytes depending on the magnitude."""
    if n < 24:
        return bytes([major | n])
    if n < 0x100:
        return bytes([major | 24, n])
    if n < 0x10000:
        return bytes([major | 25]) + struct.pack(">H", n)
    return bytes([major | 26]) + struct.pack(">I", n)


def cbor_encode(obj):
    if isinstance(obj, bool):
        return bytes([0xF5 if obj else 0xF4])
    if isinstance(obj, int):
        return _head(0x00, obj)
    if isinstance(obj, bytes):
        return _head(0x40, len(obj)) + obj
    if isinstance(obj, str):
        b = obj.encode()
        return _head(0x60, len(b)) + b
    if isinstance(obj, dict):
        out = _head(0xA0, len(obj))
        for k, v in obj.items():
            out += cbor_encode(k) + cbor_encode(v)
        return out
    if isinstance(obj, list):
        out = _head(0x80, len(obj))
        for v in obj:
            out += cbor_encode(v)
        return out
    raise TypeError(f"cannot encode {type(obj)}")


def cbor_decode(buf, i=0):
    """Return (value, next_index). Enough of CBOR to read mcumgr's replies."""
    b = buf[i]
    major, extra = b & 0xE0, b & 0x1F
    i += 1

    # Major 7 carries the simple values, and its argument is NOT a length --
    # false/true/null are 20/21/22, all below 24, so they have to be taken here
    # before the length decoding below claims them.
    if major == 0xE0:
        if extra in (20, 21):
            return extra == 21, i
        if extra == 22:
            return None, i
        raise ValueError(f"unsupported CBOR simple value {extra}")

    # Indefinite length: items run until a 0xFF break. zcbor emits maps and
    # lists this way unless ZCBOR_CANONICAL is defined, which Zephyr does not
    # define, so EVERY map the board sends arrives in this form.
    if extra == 31:
        if major == 0x80:
            out = []
            while buf[i] != 0xFF:
                v, i = cbor_decode(buf, i)
                out.append(v)
            return out, i + 1
        if major == 0xA0:
            out = {}
            while buf[i] != 0xFF:
                k, i = cbor_decode(buf, i)
                v, i = cbor_decode(buf, i)
                out[k] = v
            return out, i + 1
        raise ValueError(f"indefinite length is not supported for major type {major:#x}")

    if extra < 24:
        n = extra
    elif extra == 24:
        n, i = buf[i], i + 1
    elif extra == 25:
        n, i = struct.unpack_from(">H", buf, i)[0], i + 2
    elif extra == 26:
        n, i = struct.unpack_from(">I", buf, i)[0], i + 4
    else:
        raise ValueError(f"unsupported CBOR additional info {extra}")

    if major == 0x00:
        return n, i
    if major == 0x20:
        return -1 - n, i
    if major == 0x40:
        return buf[i : i + n], i + n
    if major == 0x60:
        return buf[i : i + n].decode(), i + n
    if major == 0x80:
        out = []
        for _ in range(n):
            v, i = cbor_decode(buf, i)
            out.append(v)
        return out, i
    if major == 0xA0:
        out = {}
        for _ in range(n):
            k, i = cbor_decode(buf, i)
            v, i = cbor_decode(buf, i)
            out[k] = v
        return out, i
    raise ValueError(f"unsupported CBOR major type {major:#x}")


# ---- SMP over GATT -----------------------------------------------------------


# ---- mcumgr's serial framing ------------------------------------------------
#
# THE CABLE IS THE SAME PROTOCOL, and only this much stands between them. A GATT
# characteristic is a datagram: one write is one packet, and the controller
# fragments. A serial port is a byte stream with no packet boundaries at all, so
# mcumgr defines its own framing on top, and this is that framing.
#
# Transcribed from zephyr/subsys/mgmt/mcumgr/transport/src/serial_util.c and
# zephyr/include/zephyr/mgmt/mcumgr/transport/serial.h:
#
#   packet   be16(len(body) + 2) || body || be16(crc16(body))
#   frame    be16(marker) || base64(slice of packet) || b"\n"
#   marker   0x0609 on a packet's first frame, 0x0414 on every continuation
#   limit    127 bytes per frame INCLUDING the markers and the newline
#
# The length word counts the CRC but not itself. A receiver checks a packet by
# running the same CRC over the body AND its trailing CRC and requiring zero.
#
# binascii.crc_hqx(data, 0) IS CRC-16/XMODEM -- poly 0x1021, init 0x0000, not
# reflected -- which is exactly what serial_util.c:30 calls crc16_itu_t(0, ...).
# Using the standard library rather than hand-rolling it is the point: this file
# is the reference the browser's copy is diffed against, so the fewer things in
# it that are mine, the more that diff is worth.
SERIAL_HDR_PKT = 0x0609
SERIAL_HDR_FRAG = 0x0414
SERIAL_MAX_FRAME = 127
SERIAL_FIRST_INPUT = ((SERIAL_MAX_FRAME - 3) >> 2) * 3   # 93
SERIAL_NEXT_INPUT = SERIAL_FIRST_INPUT - 3               # 90


def serial_encode(body):
    """Wrap one SMP frame into the line-oriented frames mcumgr expects."""
    crc = binascii.crc_hqx(body, 0)
    pkt = struct.pack(">H", len(body) + 2) + body + struct.pack(">H", crc)

    out = bytearray()
    off, first = 0, True
    while off < len(pkt):
        take = min(SERIAL_FIRST_INPUT if first else SERIAL_NEXT_INPUT, len(pkt) - off)
        out += struct.pack(">H", SERIAL_HDR_PKT if first else SERIAL_HDR_FRAG)
        out += base64.b64encode(pkt[off : off + take])
        out += b"\n"
        off += take
        first = False
    return bytes(out)


class SerialFramer:
    """Reassembles SMP frames out of a byte stream.

    TOLERATES NOISE, and has to: uart0 carries MCUboot's banner, whatever a
    previous session left half-written, and anything the board printed before
    this process opened the port. So an unrecognised line is dropped rather than
    raised, and a packet that fails its CRC is dropped rather than left
    half-built to poison the next one.
    """

    def __init__(self):
        self.line = bytearray()
        self.pkt = bytearray()
        self.want = 0
        self.overrun = False
        self.last = -1

    def feed(self, chunk):
        done = []
        for byte in chunk:
            # RESYNC ON A PACKET MARKER, wherever it turns up. Everything after
            # a frame's two marker bytes is base64, and the base64 alphabet is
            # A-Za-z0-9+/= -- so 0x06 0x09 CANNOT occur inside a well-formed
            # frame. Seeing it means a new packet started here, whatever was
            # being accumulated before. Without this, a long run of output with
            # no newline in it swallows the next real frame too, because the two
            # are one line as far as a newline-delimited reader can tell.
            if self.last == 0x06 and byte == 0x09:
                self.overrun = False
                self.line = bytearray(b"\x06\x09")
                self.last = byte
                continue
            self.last = byte

            if byte == 0x0A:                       # end of a frame
                if self.overrun:
                    self.overrun = False
                    self.line = bytearray()
                    continue
                pkt = self._line(bytes(self.line))
                self.line = bytearray()
                if pkt is not None:
                    done.append(pkt)
            elif byte != 0x0D:                     # ignore the CR in CRLF
                if self.overrun:
                    continue
                # A frame is 127 bytes at most. Anything longer is not one, and
                # letting it grow turns a chatty port into a memory leak.
                if len(self.line) < SERIAL_MAX_FRAME:
                    self.line.append(byte)
                else:
                    self.overrun = True
                    self.line = bytearray()
        return done

    def _line(self, line):
        if len(line) < 2:
            return None
        marker = struct.unpack_from(">H", line, 0)[0]

        if marker == SERIAL_HDR_PKT:
            self.pkt = bytearray()
            self.want = 0
        elif marker != SERIAL_HDR_FRAG:
            return None                            # console noise, not a frame
        elif not self.pkt and self.want == 0:
            return None                            # continuation with no start

        try:
            self.pkt += base64.b64decode(line[2:], validate=True)
        except (ValueError, binascii.Error):
            self._reset()
            return None

        if marker == SERIAL_HDR_PKT:
            if len(self.pkt) < 2:
                return None
            self.want = struct.unpack_from(">H", self.pkt, 0)[0]
            del self.pkt[:2]

        if self.want == 0 or len(self.pkt) < self.want:
            return None
        if len(self.pkt) > self.want:
            self._reset()
            return None

        full = bytes(self.pkt)
        self._reset()

        # CRC over the body AND its trailing CRC comes out zero when it checks.
        if binascii.crc_hqx(full, 0) != 0:
            return None
        return full[:-2]

    def _reset(self):
        self.pkt = bytearray()
        self.want = 0


# ---- transports -------------------------------------------------------------
#
# `Smp` below talks to one of these rather than to a BLE client directly. The
# interface is one method and one number: `send(frame)` and `chunk`. Everything
# above it -- header, CBOR, image group, upload loop -- is identical either way,
# which is the whole claim the cable path rests on.


class BleTransport:
    """A notifying SMP characteristic."""

    def __init__(self, client):
        self.client = client
        # Leave room for the SMP header and the CBOR keys around the data. The
        # board's netbuf is 512 B, so this is bounded by the ATT MTU, not by it.
        mtu = getattr(client, "mtu_size", 0) or 185
        self.chunk = max(64, mtu - 80)

    async def send(self, frame):
        await self.client.write_gatt_char(SMP_CHR_UUID, frame, response=False)


class SerialTransport:
    """The board's SMP UART; on the DWM3001CDK, uart0, which is the J-Link OB's VCOM.

    TWO DIFFERENT THINGS LISTEN HERE, at different times, and this does not care
    which answered: the application, through CONFIG_MCUMGR_TRANSPORT_UART, and
    MCUboot, through CONFIG_MCUBOOT_SERIAL. Same framing for both.
    """

    def __init__(self, port, baud, chunk):
        try:
            import serial                                      # noqa: PLC0415
        except ImportError:
            die("pyserial is not installed. Fix: pip install ./tools/tinyfota")
        try:
            self.ser = serial.Serial(port, baud, timeout=0.05)
        except Exception as exc:                                # noqa: BLE001
            die(f"cannot open {port}: {exc}")
        self.chunk = chunk
        self.framer = SerialFramer()
        self.stop = False

    async def send(self, frame):
        self.ser.write(serial_encode(frame))
        self.ser.flush()

    async def pump(self, smp):
        """Feed the conversation, until cancelled."""
        while not self.stop:
            data = await asyncio.to_thread(self.ser.read, 4096)
            if not data:
                continue
            for pkt in self.framer.feed(data):
                # Whole packets, where BLE delivers pieces. on_notify
                # reassembles by the header's own length either way.
                smp.on_notify(None, pkt)

    def close(self):
        self.stop = True
        try:
            self.ser.close()
        except Exception:                                       # noqa: BLE001, S110
            pass


class Smp:
    """One mcumgr conversation. Reassembles responses, matches them by seq."""

    def __init__(self, transport):
        """Bind one conversation to a transport. Tracks the outgoing sequence
        number, buffers incoming chunks, and queues complete frames."""
        self.t = transport
        self.seq = 0
        self.rx = bytearray()
        self.frames = asyncio.Queue()

    def on_notify(self, _sender, data):
        """Reassemble SMP response frames from BLE notifications. Buffers data and enqueues complete frames once the length declared in the 8-byte header is satisfied."""
        # A response longer than one notification arrives in pieces with a
        # single 8-byte header at the front, so buffer until the header's
        # declared length is complete.
        self.rx += data
        while len(self.rx) >= 8:
            length = struct.unpack_from(">H", self.rx, 2)[0]
            if len(self.rx) < 8 + length:
                return
            self.frames.put_nowait(bytes(self.rx[: 8 + length]))
            del self.rx[: 8 + length]

    async def call(self, op, group, cmd_id, payload, timeout=20.0):
        body = cbor_encode(payload)
        self.seq = (self.seq + 1) & 0xFF
        hdr = struct.pack(">BBHHBB", op, 0, len(body), group, self.seq, cmd_id)

        await self.t.send(hdr + body)

        while True:
            try:
                frame = await asyncio.wait_for(self.frames.get(), timeout)
            except asyncio.TimeoutError:
                die("the board stopped answering")
            if frame[6] == self.seq:
                break

        rsp, _ = cbor_decode(frame, 8) if len(frame) > 8 else ({}, 0)
        rc = rsp.get("rc", 0)
        if rc:
            die(f"refused: rc={rc} ({MGMT_ERR.get(rc, 'unknown')})")
        return rsp


async def converse(smp, args):
    """The whole exchange, once something is listening.

    TRANSPORT-INDEPENDENT ON PURPOSE. The radio and the cable differ in how a
    board is found and how bytes leave; they do not differ in the image list,
    the signature check, the update window, the upload loop or the reset. If
    this function ever needs to know which one it is on, something above it has
    been built wrong.
    """
    state = await smp.call(OP_READ_REQ, GRP_IMG, IMG_ID_STATE, {})
    for img in state.get("images", []):
        print(
            f"  slot {img.get('slot')}: v{img.get('version')} "
            f"sha={img.get('hash', b'')[:8].hex()} "
            f"active={img.get('active')} confirmed={img.get('confirmed')}"
        )
    if args.expect:
        want = image_sha(args.expect)
        got = next((i.get("hash", b"") for i in state.get("images", [])), b"")
        if got != want:
            die(
                f"the board is NOT running that image.\n"
                f"  board  {got.hex()}\n"
                f"  wanted {want.hex()}\n"
                f"  The update did not land, so the deployed record must not move:\n"
                f"  a delta built from the wrong base is refused by the board."
            )
        print(f"  confirmed: the board is running {want[:8].hex()}...")
        return

    if args.list:
        return

    blob = Path(args.patch).read_bytes()
    whole_image = False
    if blob[:4] == b"WDFU":
        print("  raw .wdfu")
    elif struct.unpack_from("<I", blob, 0)[0] == 0x96F3B83D:
        hdr_sz, img_sz = struct.unpack_from("<H", blob, 8)[0], struct.unpack_from("<I", blob, 12)[0]
        if blob[hdr_sz : hdr_sz + 4] == b"WDFU":
            # The phone-facing file. Sending it here is the point: it exercises
            # the board's wrapper-skipping on exactly the bytes the app sends.
            print(f"  mcuboot-wrapped: {hdr_sz} B header + {img_sz} B patch "
                  f"+ {len(blob)-hdr_sz-img_sz} B TLV")
        else:
            # A WHOLE APPLICATION IMAGE, which is a different thing entirely and
            # used to be rejected here as a malformed wrapper. It is what
            # MCUboot's serial recovery takes: no starting image, no delta, the
            # entire slot. Refusing it meant this script could not drive the one
            # path that reaches a board whose application does not boot.
            whole_image = True
            print(f"  whole MCUboot image: {hdr_sz} B header + {img_sz} B application "
                  f"+ {len(blob)-hdr_sz-img_sz} B TLV")
            if not args.serial:
                die(
                    "a whole image goes to MCUboot's serial recovery, which is only\n"
                    "  reachable over the cable. Add --serial PORT, and hold the update\n"
                    "  button for five seconds first so the board reboots into the\n"
                    "  bootloader.\n"
                    "  (Over the radio the application takes a delta, not an image.)"
                )
    else:
        die(f"{args.patch} is neither a .wdfu nor an MCUboot image. Fix: tinyfota build")

    # MCUboot's receive buffer is smaller than the application's, and a request
    # it cannot hold is dropped in silence rather than refused -- which reads as
    # a board that stopped talking. Say so rather than letting it look like the
    # board's fault.
    if whole_image and smp.t.chunk > 512:
        print(f"  note: --chunk {smp.t.chunk} is above what MCUboot's 1024-byte receive\n"
              f"        buffer leaves room for once framing is counted. If the\n"
              f"        bootloader stops answering mid-upload, drop to 384.")

    chunk = smp.t.chunk
    print(f"pushing {len(blob)} B in {chunk} B chunks")
    off = 0
    while off < len(blob):
        body = {"image": 0, "off": off}
        if off == 0:
            body["len"] = len(blob)
        body["data"] = blob[off : off + chunk]

        rsp = await smp.call(OP_WRITE_REQ, GRP_IMG, IMG_ID_UPLOAD, body)
        nxt = rsp.get("off")
        if nxt is None:
            die("no off in the upload response")
        if nxt == off:
            die("the board is not advancing; it refused the chunk silently")
        off = nxt
        print(f"\r  {off}/{len(blob)} B", end="", flush=True)
    print("\n staged.")

    if args.no_reset:
        print(" not resetting (--no-reset). The patch applies at the next boot.")
        return

    if whole_image:
        print(" resetting; the board boots straight into the image just written")
    else:
        print(" resetting; MCUboot applies the patch before the board answers again")
    try:
        await smp.call(OP_WRITE_REQ, GRP_OS, OS_ID_RESET, {}, timeout=5.0)
    except SystemExit:
        # A board that reboots before answering is the expected case, not a
        # failure: the reset response races the reset itself.
        pass


async def run_serial(args):
    """Talk to whatever is listening on uart0.

    NO SCAN AND NO CHOOSER, which is most of why this path is worth having: a
    cable is already an unambiguous answer to "which board". It also reaches a
    board the radio cannot -- MCUboot's serial recovery listens on this same
    port when the application is not running at all.
    """
    print(f"opening {args.serial} at {args.baud}")
    transport = SerialTransport(args.serial, args.baud, args.chunk)
    smp = Smp(transport)
    pump = asyncio.create_task(transport.pump(smp))
    try:
        await converse(smp, args)
    finally:
        pump.cancel()
        try:
            await pump
        except asyncio.CancelledError:
            pass
        transport.close()


class Sightings:
    """Every service UUID each device has shown, over the whole scan.

    CUMULATIVE ON PURPOSE. A board that keeps its advertisement for something
    else puts the SMP UUID in its scan response (the door-lock on tinyble
    does), and the advertisement and the scan response arrive as separate
    reports. A scan that keeps only each device's latest report, as
    BleakScanner.discover() does, loses the UUID whenever the last report
    seen was the advertisement. Collecting the union of every report keeps it.
    """

    def __init__(self, shorts):
        self.shorts = shorts
        self.uuids = {}
        self.devices = {}

    def see(self, dev, adv):
        u = self.uuids.setdefault(dev.address, set())
        u |= {x.lower() for x in (adv.service_uuids or [])}
        u |= {x.lower() for x in (adv.service_data or {})}
        # The newest device object: it carries the newest name.
        self.devices[dev.address] = dev

    def boards(self, address=""):
        out = []
        for addr, uuids in self.uuids.items():
            if not any(s in u for s in self.shorts for u in uuids):
                continue
            if address and addr.lower() != address.lower():
                continue
            out.append(self.devices[addr])
        return out


async def find_board(args, shorts, what):
    """Scan for args.scan seconds; the one board showing a UUID in shorts."""
    from bleak import BleakScanner

    print("scanning...")
    seen = Sightings(shorts)
    async with BleakScanner(detection_callback=seen.see):
        await asyncio.sleep(args.scan)
    boards = seen.boards(args.address)
    if not boards:
        die(f"no board advertising {what} was found. Is it powered and in range?")
    if len(boards) > 1:
        listed = "\n".join(f"    {d.address}  {d.name or ''}" for d in boards)
        die(f"{len(boards)} boards advertise {what}; pick one with --address:\n{listed}")
    return boards[0]


# ---- the native frames --------------------------------------------------------


class Native:
    """One conversation in the receiver's own frames: a frame out, a reply back.

    Every frame is idempotent by design (core/dfu_receiver.c): a BEGIN that
    matches the transfer in progress is answered with its position, and a DATA
    at any offset other than the receiver's is answered with the receiver's
    offset and changes nothing. So a lost reply costs a resend, never a
    corrupted transfer.
    """

    def __init__(self, client, mtu):
        self.client = client
        self.replies = asyncio.Queue()
        # One DATA frame per write: the ATT payload is MTU - 3, and the frame
        # spends 9 B on its opcode, transfer id and offset.
        self.room = max(1, mtu - 3 - 9)
        self.timeout = 10.0

    def on_notify(self, _sender, data):
        self.replies.put_nowait(bytes(data))

    async def call(self, frame, tries=3):
        """Send one frame and return its reply, resending on silence."""
        for _ in range(tries):
            while not self.replies.empty():
                self.replies.get_nowait()          # a late reply to an old frame
            await self.client.write_gatt_char(NATIVE_CHR_UUID, frame, response=False)
            try:
                return await asyncio.wait_for(self.replies.get(), self.timeout)
            except asyncio.TimeoutError:
                continue
        die("the board stopped answering")


def native_ok(rsp, transfer_id):
    """The next offset from an OK reply, or the process ends with the board's reason."""
    if len(rsp) == 2 and rsp[0] == NATIVE_ERR:
        die(f"refused: {NATIVE_ERRORS.get(rsp[1], f'error {rsp[1]}')}")
    if len(rsp) != 9 or rsp[0] != NATIVE_OK:
        die(f"unexpected reply {rsp.hex()}")
    tid, nxt = struct.unpack_from("<II", rsp, 1)
    if tid != transfer_id:
        die(f"reply for transfer {tid:#x}, not {transfer_id:#x}")
    return nxt


def native_payload(blob):
    """The bytes the receiver takes: a raw .wdfu, or the one inside a wrapped file."""
    if blob[:4] == b"WDFU":
        return blob
    if len(blob) >= 16 and struct.unpack_from("<I", blob, 0)[0] == 0x96F3B83D:
        hdr_sz, img_sz = struct.unpack_from("<H", blob, 8)[0], struct.unpack_from("<I", blob, 12)[0]
        inner = blob[hdr_sz : hdr_sz + img_sz]
        if inner[:4] == b"WDFU":
            # SMP steps over the wrapper on the board; the native frames do
            # not, so it comes off here.
            return inner
        die("a whole MCUboot image goes to MCUboot's serial recovery: --serial, not --native")
    die("neither a .wdfu nor a wrapped one. Fix: tinyfota build")


async def push_native(native, blob, transfer_id=None):
    """BEGIN, DATA from wherever the board says it is, COMMIT. Returns frames sent."""
    if transfer_id is None:
        transfer_id = int.from_bytes(os.urandom(4), "little") or 1
    total = len(blob)
    frames = 0

    off = native_ok(await native.call(struct.pack("<BII", NATIVE_BEGIN, transfer_id, total)),
                    transfer_id)
    frames += 1
    print(f"pushing {total} B in {native.room} B frames")
    stuck = 0
    while off < total:
        chunk = blob[off : off + native.room]
        nxt = native_ok(await native.call(struct.pack("<BII", NATIVE_DATA, transfer_id, off)
                                          + chunk), transfer_id)
        frames += 1
        if nxt > total:
            die(f"the board reports offset {nxt}, past the {total} B sent")
        # Anything but off + len(chunk) is the board saying where it really
        # is, after a lost frame or reply: resend from there.
        stuck = stuck + 1 if nxt <= off else 0
        if stuck >= 3:
            die("the board is not advancing; it refused the frame silently")
        off = nxt
        print(f"\r  {off}/{total} B", end="", flush=True)
    print()
    native_ok(await native.call(struct.pack("<BI", NATIVE_COMMIT, transfer_id)), transfer_id)
    frames += 1
    print(" staged; the board restarts by itself and MCUboot applies the patch")
    return frames


async def run_native(args):
    try:
        from bleak import BleakClient
    except ImportError:
        die("bleak is not installed. Fix: pip install ./tools/tinyfota")

    blob = native_payload(Path(args.patch).read_bytes())
    device = await find_board(args, (SMP_SVC_SHORT, NATIVE_SVC_SHORT), "the SMP or native service")
    print(f"connecting to {device.name or device.address}")
    # services=[...] for the reason given in run() below.
    async with BleakClient(device, services=[NATIVE_SVC_UUID]) as client:
        native = Native(client, getattr(client, "mtu_size", 0) or 23)
        await client.start_notify(NATIVE_CHR_UUID, native.on_notify)
        await push_native(native, blob)
        # COMMIT restarts the board; the link drops under the context exit.
        await asyncio.sleep(1.0)


async def run(args):
    if args.serial:
        await run_serial(args)
        return
    if args.native:
        await run_native(args)
        return

    try:
        from bleak import BleakClient
    except ImportError:
        die("bleak is not installed. Fix: pip install ./tools/tinyfota")

    # The SMP service itself, in the UUID list or the service data.
    # Unambiguous: it is the exact service this tool is about to talk to.
    device = await find_board(args, (SMP_SVC_SHORT,), "the SMP service")

    print(f"connecting to {device.name or device.address}")
    # services=[...] on purpose. In the source project a third-party descriptor
    # discovery on a service macOS reserves for the system came back as
    # CBError 8 and took the whole discovery down; restricting discovery to the
    # one service this tool needs is what makes the CLI work whatever else the
    # board publishes.
    async with BleakClient(device, services=[SMP_SVC_UUID]) as client:
        smp = Smp(BleTransport(client))
        await client.start_notify(SMP_CHR_UUID, smp.on_notify)
        await converse(smp, args)


def add_arguments(ap):
    """The `push` options, shared by `tinyfota push` and this module's main()."""
    ap.add_argument("patch", nargs="?", help="the .wdfu from tinyfota build, or its wrapped .bin")
    ap.add_argument("--list", action="store_true", help="read the image list and stop")
    ap.add_argument(
        "--expect",
        metavar="SIGNED_BIN",
        help="check the board is running this image's SHA-256, then stop",
    )
    ap.add_argument("--address", default="",
                    help="the board to use when more than one advertises the SMP service")
    # 12 s, not 8. MEASURED in ultrawidelock: two consecutive 8 s scans missed a board sitting at
    # -60 dBm that a 12 s scan then found immediately. CoreBluetooth filters
    # duplicates across back-to-back discovery sessions, so a board that is
    # plainly present can simply not be reported to a short window -- and the
    # failure reads as "the board is not advertising", which is the wrong
    # conclusion entirely.
    ap.add_argument("--scan", type=float, default=12.0, help="scan seconds")
    ap.add_argument("--no-reset", action="store_true", help="stage without rebooting")
    # The receiver's own frames instead of SMP: BEGIN, DATA sized to the ATT
    # MTU, COMMIT, which restarts the board itself. Bluetooth only.
    ap.add_argument("--native", action="store_true",
                    help="push over tinyfota's native GATT frames (d3b5a141) instead of SMP")
    # THE CABLE. On the DWM3001CDK uart0 is the J-Link OB's VCOM, so this is the
    # port the probe already presents -- no second cable, and no scan.
    ap.add_argument(
        "--serial",
        metavar="PORT",
        default="",
        help="talk over a serial port instead of Bluetooth, e.g. /dev/cu.usbmodem<serial>",
    )
    ap.add_argument("--baud", type=int, default=115200,
                    help="serial rate; MCUboot and the application both use 115200")
    # 384 leaves room under CONFIG_MCUMGR_TRANSPORT_UART_MTU=512 for the 8-byte
    # header and the CBOR map. It suits MCUboot too, whose ceiling is HIGHER
    # than the application's -- BOOT_SERIAL_MAX_RECEIVE_SIZE is 1024 -- and not
    # lower as this once claimed. Measured in ultrawidelock against a board in serial recovery:
    # 128 runs at ~500 B/s and 384 at ~1.6 KB/s, which on a 406 KB image is 13
    # minutes against 4.
    ap.add_argument("--chunk", type=int, default=384,
                    help="serial upload payload per request; suits both the "
                         "application and MCUboot")


def push(args):
    """`tinyfota push`: the whole exchange, over Bluetooth or the cable."""
    if not args.list and not args.expect and not args.patch:
        die("give a patch file, or --list, or --expect")
    if args.native and (args.list or args.expect or args.serial or args.no_reset or not args.patch):
        die("--native pushes a patch over Bluetooth, and COMMIT restarts the board: "
            "--list, --expect, --serial and --no-reset are SMP's")
    asyncio.run(run(args))


def add_parser(sub):
    p = sub.add_parser("push", help="push a patch over SMP, by Bluetooth or serial, "
                                    "as a phone would")
    add_arguments(p)
    p.set_defaults(func=push)


def main():
    ap = argparse.ArgumentParser(description="Push a delta patch over SMP, as a phone would.")
    add_arguments(ap)
    push(ap.parse_args())


if __name__ == "__main__":
    main()
