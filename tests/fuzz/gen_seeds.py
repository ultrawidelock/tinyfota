#!/usr/bin/env python3
"""Write the seed corpora for the libFuzzer targets in tests/fuzz/.

    python3 tests/fuzz/gen_seeds.py tests/fuzz/corpus

Needs a Python with tools/tinyfota installed (it brings detools): the apply
seeds are real in-place patches, made with the same detools settings
`tinyfota build` uses, against the base image fuzz_apply.c lays down. Seeds
are a starting point, not a test: libFuzzer mutates from them, so the deeper
paths (a staged header, a resumed apply) are reached in seconds rather than
never. The output is deterministic, so regenerating changes nothing.
"""
import io
import struct
import sys
import zlib
from pathlib import Path

import detools
from tinyfota.patch import (
    HDR_FMT,
    HEATSHRINK_LOOKAHEAD_SZ2,
    HEATSHRINK_WINDOW_SZ2,
    SEGMENT_SIZE,
    TINYFOTA_ABI_VERSION,
    TINYFOTA_MAGIC,
    TINYFOTA_SIG_LEN,
)

# tests/host/port/tinyfota_host.h: the primary slot the host port models.
PRIMARY_SIZE = 0x6A000
# fuzz_apply.c: FUZZ_BASE_LEN and base_byte().
BASE_LEN = 16384


def base_image():
    return bytes(((i * 2654435761) & 0xFFFFFFFF) >> 24 for i in range(BASE_LEN))


def in_place_patch(src, dst):
    out = io.BytesIO()
    detools.create_patch(
        io.BytesIO(src),
        io.BytesIO(dst),
        out,
        patch_type="in-place",
        compression="heatshrink",
        memory_size=PRIMARY_SIZE,
        segment_size=SEGMENT_SIZE,
        heatshrink_window_sz2=HEATSHRINK_WINDOW_SZ2,
        heatshrink_lookahead_sz2=HEATSHRINK_LOOKAHEAD_SZ2,
    )
    return out.getvalue()


def header(patch, to_len, from_data):
    body = struct.pack(HDR_FMT, TINYFOTA_MAGIC, TINYFOTA_ABI_VERSION, 0, len(patch), to_len,
                       zlib.crc32(patch), zlib.crc32(from_data), len(from_data))
    return body + struct.pack("<I", zlib.crc32(body))


def main(out_dir):
    out = Path(out_dir)
    base = base_image()
    edited = bytearray(base)
    edited[5000:5010] = b"tinyfota!!"
    targets = {
        "edit": bytes(edited),
        "grow": base + bytes(range(256)) * 8,
        "shift": b"\x00" * 100 + base,
    }
    patches = {name: in_place_patch(base, t) for name, t in targets.items()}

    def put(target, name, blob):
        d = out / target
        d.mkdir(parents=True, exist_ok=True)
        (d / f"seed-{name}").write_bytes(blob)

    # fuzz_apply: [to_len:4][steps:1][patch]
    for name, patch in patches.items():
        put("apply", name, struct.pack("<IB", len(targets[name]), 0) + patch)
    put("apply", "edit-resume", struct.pack("<IB", len(targets["edit"]), 2) + patches["edit"])

    # A preamble and patch a receiver accepts with every fix-up off: a real
    # header naming no base (from_len 0), a placeholder signature (the fuzz
    # port accepts any), and a real patch.
    patch = patches["edit"]
    wire = header(patch, len(targets["edit"]), b"") + b"\xa5" * TINYFOTA_SIG_LEN + patch

    # fuzz_header: [fix][preamble + patch]
    put("header", "valid", b"\x00" + wire)
    put("header", "fixups", b"\x1f" + wire)

    # fuzz_frame: [fix] then [owner][len][frame] records (owner 4 is TEST)
    tid = struct.pack("<I", 0x5EED5EED)
    recs = bytearray(b"\x00")
    begin = b"\x11" + tid + struct.pack("<I", len(wire))
    recs += bytes([4, len(begin)]) + begin
    for off in range(0, len(wire), 244):
        f = b"\x12" + tid + struct.pack("<I", off) + wire[off:off + 244]
        recs += bytes([4, len(f)]) + f
    commit = b"\x13" + tid
    recs += bytes([4, len(commit)]) + commit
    put("frame", "staged", bytes(recs))

    # fuzz_upload: [fix] then [off:4][total:4][len:2][chunk] records
    recs = bytearray(b"\x00")
    for off in range(0, len(wire), 512):
        chunk = wire[off:off + 512]
        recs += struct.pack("<IIH", off, len(wire), len(chunk)) + chunk
    put("upload", "staged", bytes(recs))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: gen_seeds.py <corpus dir>")
    main(sys.argv[1])
