#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Aseem Chopra
# SPDX-License-Identifier: Apache-2.0
"""Write the power-cut simulator's fixtures: base.bin, target.bin, update.wdfu.

    python3 tests/powercut/gen_fixtures.py tests/powercut/fixtures

The images are MCUboot-shaped (32 B header padded to 512 B, a body, a TLV block
holding the SHA-256) but the body is deterministic pseudo-random bytes, not
firmware. The target differs from the base the way a real rebuild does: an
insertion and a deletion that shift everything after them (net -2 B), three
rewritten regions, and scattered single-word edits. Sizes match the sample's
slot (samples/nrf52833/pm_static.yml): the images fill 191 KB of a 424 KB slot.

base.bin and target.bin are the same on every run. update.wdfu is signed with a
throwaway key, so its signature differs on every run; powercut.c does not verify
it. Needs `pip install tools/tinyfota`.
"""

import hashlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

IMAGE_MAGIC = 0x96F3B83D
TLV_INFO_MAGIC = 0x6907
TLV_SHA256 = 0x10
HDR_SIZE = 512
BODY_SIZE = 190720
PRIMARY_SIZE = 0x6A000
STAGING_SIZE = 0x8000


def stream(tag, n):
    """n deterministic bytes: SHA-256 in counter mode over a tag."""
    out = bytearray()
    i = 0
    while len(out) < n:
        out += hashlib.sha256(b"%s:%d" % (tag.encode(), i)).digest()
        i += 1
    return bytes(out[:n])


def image(body, build):
    hdr = struct.pack("<IIHHII", IMAGE_MAGIC, 0, HDR_SIZE, 0, len(body), 0)
    hdr += struct.pack("<BBHI", 1, 0, 0, build)
    hdr = hdr.ljust(HDR_SIZE, b"\0")
    sha = hashlib.sha256(hdr + body).digest()
    tlv = struct.pack("<HH", TLV_INFO_MAGIC, 4 + 4 + len(sha))
    tlv += struct.pack("<BBH", TLV_SHA256, 0, len(sha)) + sha
    return hdr + body + tlv


def target_body(base):
    b = bytearray(base)
    for off, n, tag in ((20000, 300, "rewrite-a"), (90000, 700, "rewrite-b"), (150000, 1200, "rewrite-c")):
        b[off : off + n] = stream(tag, n)
    words = stream("words", 4 * 48)
    pos = stream("pos", 4 * 48)
    for i in range(48):
        at = (struct.unpack_from("<I", pos, 4 * i)[0] % (len(b) // 4)) * 4
        b[at : at + 4] = words[4 * i : 4 * i + 4]
    b[60000:60000] = stream("insert", 36)
    del b[120000:120038]
    return bytes(b)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    base_body = stream("base", BODY_SIZE)
    (out / "base.bin").write_bytes(image(base_body, 1))
    (out / "target.bin").write_bytes(image(target_body(base_body), 2))

    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    key = ec.generate_private_key(ec.SECP256R1()).private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    with tempfile.TemporaryDirectory() as tmp:
        pem = Path(tmp) / "throwaway.pem"
        pem.write_bytes(key)
        subprocess.run(
            [
                sys.executable, "-m", "tinyfota", "build",
                "--from", str(out / "base.bin"), "--to", str(out / "target.bin"),
                "--memory-size", str(PRIMARY_SIZE), "--staging-size", str(STAGING_SIZE),
                "--key", str(pem), "--out", str(out / "update.wdfu"),
            ],
            check=True,
        )


if __name__ == "__main__":
    main()
