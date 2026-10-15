#!/usr/bin/env python3
"""Emit the wire vectors fota_wire_check.mjs compares the browser code against.

    fota_wire_vectors.py <out.json> <repo-root>

Everything here comes from the SHIPPING tool -- tools/tinyfota/tinyfota/smp.py's
own CBOR encoder -- and from the frame formats in FORMAT.md, which the receiver
(core/dfu_receiver.c) parses, so a change to either shows up as a failure rather
than being mirrored into the expectation.

The indefinite-length maps are built by hand because nothing on this side ever
produces them: zcbor emits them (Zephyr does not define ZCBOR_CANONICAL), so
they only exist in replies FROM a board. They are the shape a naive decoder
silently drops, which makes them the most valuable vector in the file.
"""

import importlib.util
import json
import pathlib
import struct
import sys


def load(name, path):
    """Import a script that is not on the path and whose name is not a module."""
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    out_path = pathlib.Path(sys.argv[1])
    repo = pathlib.Path(sys.argv[2])
    smp = load("tinyfota_smp", repo / "tools" / "tinyfota" / "tinyfota" / "smp.py")

    # Definite-length maps: what the page SENDS. Every request shape the upload
    # loop and the image-list read can produce, plus the integer-width
    # boundaries CBOR changes encoding at (23/24, 255/256, 65535/65536).
    cases = [
        {},
        {"image": 0, "off": 0},
        {"image": 0, "off": 105, "len": 11264},
        {"image": 0, "off": 4294967295},
        {"rc": 11},
        {"bool_true": True, "bool_false": False},
        {"s": "tinyfota"},
        {"nested": {"a": 1, "b": {"c": 2}}},
        {"twentythree": 23, "twentyfour": 24, "twofivefive": 255,
         "twofivesix": 256, "sixtyfivefivethreefive": 65535,
         "sixtyfivefivethreesix": 65536},
    ]

    # Byte strings travel as hex because JSON cannot carry them. 300 bytes is
    # past the 1-byte length prefix, which is where a length encoder goes wrong.
    byte_cases = [b"", b"\x00", b"WDFU", bytes(range(256)), b"\xff" * 300]

    class Raw(bytes):
        """Already-encoded CBOR, to be spliced rather than encoded again.

        Without this a nested indefinite map handed in as bytes comes back out
        as a CBOR BYTE STRING containing those bytes -- which decodes cleanly,
        looks plausible, and is not the structure a board sends. The first draft
        of this file made exactly that mistake.
        """

    def enc(v):
        return bytes(v) if isinstance(v, Raw) else smp.cbor_encode(v)

    def indef_map(pairs):
        out = b"\xbf"
        for k, v in pairs:
            out += smp.cbor_encode(k) + enc(v)
        return out + b"\xff"

    def indef_array(items):
        out = b"\x9f"
        for i in items:
            out += enc(i)
        return out + b"\xff"

    # The real one: an image-list reply, as a board sends it. A map containing
    # an array of maps, all three indefinite.
    image_entry = Raw(indef_map([
        ("slot", 0),
        ("version", "0.3.1"),
        ("hash", bytes(range(32))),
        ("bootable", True),
        ("active", True),
        ("confirmed", True),
    ]))
    indefinite = [
        (indef_map([("rc", 0)]).hex(), {"rc": 0}),
        (indef_map([("off", 105)]).hex(), {"off": 105}),
        (indef_map([("images", Raw(indef_array([image_entry])))]).hex(), None),
    ]

    # op, flags, len, group, seq, id -- smp.py Smp.call
    smp_headers = [
        struct.pack(">BBHHBB", 0, 0, 0, 1, 1, 0).hex(),
        struct.pack(">BBHHBB", 2, 0, 137, 1, 42, 1).hex(),
        struct.pack(">BBHHBB", 2, 0, 65535, 0, 255, 5).hex(),
    ]
    smp_header_fields = [
        {"op": 0, "len": 0, "group": 1, "seq": 1, "id": 0},
        {"op": 2, "len": 137, "group": 1, "seq": 42, "id": 1},
        {"op": 2, "len": 65535, "group": 0, "seq": 255, "id": 5},
    ]

    # FORMAT.md, the frame protocol: BEGIN/DATA are "<BII", COMMIT/ABORT are "<BI".
    dfu_frames = [
        {"kind": "begin", "id": 0xDEADBEEF, "arg": 1835008,
         "hex": struct.pack("<BII", 0x11, 0xDEADBEEF, 1835008).hex()},
        {"kind": "data", "id": 0xDEADBEEF, "arg": 244,
         "hex": struct.pack("<BII", 0x12, 0xDEADBEEF, 244).hex()},
        {"kind": "commit", "id": 0xDEADBEEF, "arg": None,
         "hex": struct.pack("<BI", 0x13, 0xDEADBEEF).hex()},
        {"kind": "abort", "id": 0x00000001, "arg": None,
         "hex": struct.pack("<BI", 0x14, 0x00000001).hex()},
    ]

    # The 9-byte OK reply the JS has to parse back: opcode, id, next offset.
    dfu_ok = {"hex": struct.pack("<BII", 0x81, 0xDEADBEEF, 4096).hex(),
              "id": 0xDEADBEEF, "off": 4096}

    out_path.write_text(json.dumps({
        "cases": cases,
        "encoded": [smp.cbor_encode(c).hex() for c in cases],
        "byte_cases": [b.hex() for b in byte_cases],
        "byte_encoded": [smp.cbor_encode(b).hex() for b in byte_cases],
        "indefinite": [{"hex": h, "expect": e} for h, e in indefinite],
        "smp_headers": smp_headers,
        "smp_header_fields": smp_header_fields,
        "dfu_frames": dfu_frames,
        "dfu_ok": dfu_ok,
    }, indent=1))

    print(f"  python: {len(cases)} cbor maps, {len(byte_cases)} byte strings, "
          f"{len(indefinite)} indefinite, {len(smp_headers)} smp headers, "
          f"{len(dfu_frames)} dfu frames")


if __name__ == "__main__":
    main()
