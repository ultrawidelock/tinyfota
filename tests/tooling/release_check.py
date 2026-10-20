#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Aseem Chopra
# SPDX-License-Identifier: Apache-2.0

"""Prove `tinyfota release` never fails silently.

    python tests/tooling/release_check.py      # with tinyfota installed

Each scenario is one way the source project's release path once came up empty
or wrong without saying so (commits a2825d4e, 90b40656, 4f1ccf81 there). The
images are synthetic MCUboot images -- header, payload, a SHA-256 TLV -- which
is all `build` reads; the signing key is generated per run and thrown away.
"""

import hashlib
import os
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

MEM, STAGING = "0x6a000", "0xa000"

fails = 0
total = 0


def check(name, ok, detail=""):
    global fails, total
    total += 1
    print(f"  {'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f"  {detail}"))
    if not ok:
        fails += 1


def mcuboot_image(path, payload):
    """A minimal MCUboot image: 0x200 header, payload, TLV info + SHA-256."""
    hdr = struct.pack("<IIHHII", 0x96F3B83D, 0, 0x200, 0, len(payload), 0)
    hdr += struct.pack("<BBHI", 1, 0, 0, 0) + struct.pack("<I", 0)
    hdr = hdr.ljust(0x200, b"\xff")
    digest = hashlib.sha256(hdr + payload).digest()
    tlv = struct.pack("<HH", 0x6907, 4 + 4 + 32) + struct.pack("<BBH", 0x10, 0, 32) + digest
    Path(path).write_bytes(hdr + payload + tlv)


def run(*args, env_extra=None):
    env = dict(os.environ)
    env.pop("GITHUB_ACTIONS", None)
    env.update(env_extra or {})
    return subprocess.run([sys.executable, "-m", "tinyfota", *args], capture_output=True,
                          text=True, env=env, check=False)


def release(d, *bases, env_extra=None, key=None):
    args = ["release", "--to", str(d / "new.bin"), "--key", str(key or d / "key.pem"),
            "--memory-size", MEM, "--staging-size", STAGING, "--version", "0.2.0",
            "--out-dir", str(d / "out")]
    for b in bases:
        args += ["--base", str(b)]
    return run(*args, env_extra=env_extra)


def main():
    r = random.Random(3)
    base = bytes(r.getrandbits(8) for _ in range(60_000))
    near = base[:20_000] + b"\x00" * 200 + base[20_200:]
    far = bytes(r.getrandbits(8) for _ in range(60_000))

    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        key = ec.generate_private_key(ec.SECP256R1())
        (d / "key.pem").write_bytes(key.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
        mcuboot_image(d / "new.bin", near)
        mcuboot_image(d / "old.bin", base)
        mcuboot_image(d / "far.bin", far)

        # 1. A first release: no base, a notice, the whole image alone.
        p = release(d)
        check("first release succeeds", p.returncode == 0, p.stderr)
        check("first release says there is nothing to build a delta from",
              "nothing to build a DELTA from" in p.stdout)
        idx = (d / "out" / "ota-index.json").read_text() if p.returncode == 0 else ""
        check("first release publishes the recovery image", '"recovery"' in idx)

        # 2. One base fits, one does not: the misfit is skipped by name.
        p = release(d, d / "old.bin", d / "far.bin")
        check("a skipped base does not fail the release", p.returncode == 0, p.stderr)
        check("the skip is named at the end", "base image(s) got NO delta" in p.stdout
              and "far.bin" in p.stdout.split("got NO delta")[-1])
        skipped = d / "out" / "skipped-bases.txt"
        check("skipped-bases.txt lists it",
              skipped.is_file() and "far.bin" in skipped.read_text())
        check("one delta was published",
              len(list((d / "out" / "default").glob("*-to-*.zip"))) == 1)
        check("no GitHub annotation outside GitHub Actions", "::warning::" not in p.stdout)

        # 3. Under GitHub Actions the skip becomes an annotation on the run.
        p = release(d, d / "old.bin", d / "far.bin", env_extra={"GITHUB_ACTIONS": "true"})
        check("under GitHub Actions the skip is a ::warning::",
              p.returncode == 0 and "::warning::no delta from" in p.stdout)

        # 4. Bases given and none usable: warned, never quiet.
        p = release(d, d / "far.bin")
        check("all bases skipped still publishes", p.returncode == 0, p.stderr)
        check("and warns that the fan is empty", "none yielded a delta" in p.stdout)

        # 5. A clean run leaves no stale skipped-bases.txt behind.
        p = release(d, d / "old.bin")
        check("a clean run removes skipped-bases.txt", not skipped.exists())

        # 6. Failures that mean the inputs are wrong stop the release.
        p = release(d, d / "missing.bin")
        check("a missing base stops the release", p.returncode != 0)
        p = release(d, d / "new.bin")
        check("a base identical to the release stops it",
              p.returncode != 0 and "same image" in p.stderr)
        (d / "bad.pem").write_text("not a key")
        p = release(d, d / "old.bin", key=d / "bad.pem")
        check("an unusable key stops the release before any delta",
              p.returncode != 0 and "delta from" not in p.stdout
              and "cannot read the signing key" in p.stderr)

    if fails:
        print(f"\n  {fails} of {total} checks FAILED")
        return 1
    print(f"\nRESULT: PASS ({total} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
