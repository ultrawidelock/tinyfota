"""Build a signed delta patch for a single-slot MCUboot board.

Takes two SIGNED MCUboot images and emits one `.wdfu` file: a header the
bootloader reads, a signature the application checks, and a detools in-place
patch that turns the first image into the second.

    tinyfota build --from old/zephyr.signed.hex \\
                   --to   new/zephyr.signed.hex \\
                   --build-dir build/sample \\
                   --key  signing-key.pem \\
                   --out  update.wdfu

Why SIGNED images and not zephyr.bin: the patch has to reproduce the MCUboot
header and the ECDSA TLVs as well as the code. Patching only the payload would
leave the old signature in front of new code, and MCUboot would refuse to boot
the result -- correctly, and after the update had already overwritten the
working image.

AND IT MUST BE zephyr.signed.HEX, NOT zephyr.signed.BIN. The build signs the
image TWICE, in two separate imgtool runs, and ECDSA signatures are randomised,
so the two artifacts hold the same code under DIFFERENT signatures -- 64 bytes
apart at the very end of the image. Only the .hex goes into merged.hex and so
only the .hex is what a flashed board is actually running. MEASURED in
ultrawidelock on 2026-08-03: device crc 0xd4177b20, zephyr.signed.hex
0xd4177b20, zephyr.signed.bin 0xbb9ec396.

This does not matter for a whole-image upload, which overwrites
whatever was there. It matters completely here, because a delta is computed
AGAINST the bytes already on the device. Feed it the .bin and the bootloader
declines the patch with "not for this image" -- which is the good outcome, and
only because the from-image CRC catches it. This script refuses the .bin rather
than rely on that.

The `.wdfu` layout, which `core/tinyfota.h` is the other half of (FORMAT.md
has the whole format):

      0   32   struct tinyfota_hdr, little-endian
     32   64   ECDSA-P256 signature over those 32 bytes, raw r||s
     96   ..   the detools patch
"""

import argparse
import io
import re
import hashlib
import json
import struct
import sys
import time
import zipfile
import zlib
from pathlib import Path

# Mirrors core/tinyfota.h. A change on either side without
# the other produces a board that stages an update and then declines it, so the
# constants are named the same and checked at the bottom of this file.
TINYFOTA_MAGIC = 0x55464457  # "WDFU"
TINYFOTA_ABI_VERSION = 1
TINYFOTA_PAGE_SIZE = 4096
TINYFOTA_PATCH_OFFSET = 2 * TINYFOTA_PAGE_SIZE
TINYFOTA_HDR_LEN = 32
TINYFOTA_HDR_CRC_LEN = 28
TINYFOTA_SIG_LEN = 64
# The payload is a whole application image, not a detools delta. Occupies the
# `flags` word that was already reserved, already covered by the header CRC and
# already inside the 32 signed bytes -- so it is not an ABI change and cannot be
# flipped by anyone without the key.
TINYFOTA_FLAG_FULL_IMAGE = 0x0001

# magic, abi_version, flags, patch_len, to_len, patch_crc32, from_crc32, from_len
HDR_FMT = "<IHHIIIII"

# Heatshrink is configured statically in the bootloader, and detools.c:288-291
# REJECTS a stream whose header disagrees. These are also detools' own defaults;
# they are passed explicitly so that a change to either default is a build
# failure here rather than a patch the board refuses at its first byte.
HEATSHRINK_WINDOW_SZ2 = 8
HEATSHRINK_LOOKAHEAD_SZ2 = 7

# detools requires memory_size to be a multiple of segment_size, and the
# bootloader erases a page at a time, so a segment is a page.
SEGMENT_SIZE = 4096

# MCUboot's own image format, used by `wrap` to make a patch look like firmware.
#
# NOT because anything on the board wants it: the receiver skips straight past
# it. It is there because nRF Device Manager PARSES a file before it will offer
# to upload it, and a raw .wdfu has no magic it recognises, so the phone refuses
# the file at the picker and never gets as far as sending a byte. Wrapping costs
# 72 B and makes the file acceptable to every version of the app.
MCUBOOT_IMAGE_MAGIC = 0x96F3B83D
MCUBOOT_HDR_LEN = 32
MCUBOOT_TLV_INFO_MAGIC = 0x6907
MCUBOOT_TLV_PROT_INFO_MAGIC = 0x6908
MCUBOOT_TLV_SHA256 = 0x10
MCUBOOT_TLV_INFO_LEN = 4
MCUBOOT_TLV_HDR_LEN = 4

# The zip manifest's load_address is documentation for a human reading the
# archive, not something the board acts on. `wrap` takes it from --load-address,
# or from the mcuboot_primary partition when --build-dir is given.


def image_sha(path):
    """The SHA-256 MCUboot recorded in a signed image's TLVs.

    The same value the board prints in its image list, which is what makes it
    worth putting in a filename: comparing the two is then a glance rather than
    a tool.
    """
    d = load_image(path)
    magic, _, hdr_sz, _, img_sz, _ = struct.unpack_from("<IIHHII", d, 0)
    if magic != MCUBOOT_IMAGE_MAGIC:
        die(f"{path} is not a signed MCUboot image")

    base = hdr_sz + img_sz
    tlv_magic, tlv_tot = struct.unpack_from("<HH", d, base)
    if tlv_magic == MCUBOOT_TLV_PROT_INFO_MAGIC:
        base += tlv_tot
        tlv_magic, tlv_tot = struct.unpack_from("<HH", d, base)
    if tlv_magic != MCUBOOT_TLV_INFO_MAGIC:
        die(f"{path} has no TLV block where its header says one should be")

    p, end = base + MCUBOOT_TLV_INFO_LEN, base + tlv_tot
    while p + MCUBOOT_TLV_HDR_LEN <= end:
        kind, _, ln = struct.unpack_from("<BBH", d, p)
        p += MCUBOOT_TLV_HDR_LEN
        if kind == MCUBOOT_TLV_SHA256 and ln == 32:
            return d[p : p + 32]
        p += ln
    die(f"{path} has no SHA-256 TLV")


# A DELTA THAT DOES NOT FIT IS A FACT ABOUT TWO IMAGES, not a broken tool, and
# the caller has to be able to tell those apart by status alone.
#
# `tinyfota release` builds one delta per release still in the field. Two images
# that differ by more than the staging area holds is the single-slot design
# showing through: boards on that version have no over-the-air path, the index
# simply does not list their hash, and the page already tells them so and offers
# the whole-image reinstall instead. Failing there would take the entire release
# down -- recovery image and every delta that DID fit -- over one pair of images
# that were never going to work.
#
# Every other failure here means the release tooling is wrong and must stop the
# release. The message is the same either way; only the status differs.
# release.py's fan is the one caller that reads it.
EXIT_PATCH_TOO_BIG = 3


class PatchTooBig(SystemExit):
    """die() with EXIT_PATCH_TOO_BIG, catchable by the release fan."""


def die(msg, code=1):
    """Exit the process with the formatted error message prefixed by tinyfota."""
    sys.stdout.flush()  # so the reason lands after the progress that led to it
    print(f"tinyfota: {msg}", file=sys.stderr)
    if code == EXIT_PATCH_TOO_BIG:
        raise PatchTooBig(code)
    sys.exit(code)


def partition(build_dir, name):
    """Read one partition's size out of a build's generated partitions.yml.

    Parsed rather than hardcoded because the whole update path is invalid if
    the host and the board disagree about how big the slot is, and the
    partition map is allowed to change.
    """
    return partition_field(build_dir, name, "size")


def partition_field(build_dir, name, field):
    """One numeric field of one partition in a build's partitions.yml."""
    path = Path(build_dir) / "partitions.yml"
    if not path.is_file():
        die(f"no partitions.yml in {build_dir} -- build it first")

    text = path.read_text()
    match = re.search(rf"^{name}:$(.*?)(?=^\S)", text, re.M | re.S)
    if not match:
        die(f"no '{name}' partition in {path}")
    value = re.search(rf"^\s+{field}:\s*(0x[0-9a-fA-F]+|\d+)\s*$", match.group(1), re.M)
    if not value:
        die(f"'{name}' in {path} has no {field}")
    return int(value.group(1), 0)


def read_ihex(text):
    """Minimal Intel HEX reader. Returns the contiguous bytes it describes.

    Written here rather than pulled in as a dependency because it is twenty
    lines and this package already asks for detools and cryptography.
    """
    data = {}
    upper = 0

    for line in text.splitlines():
        line = line.strip()
        if not line.startswith(":"):
            continue
        raw = bytes.fromhex(line[1:])
        count, rectype = raw[0], raw[3]
        addr = (raw[1] << 8) | raw[2]
        payload = raw[4:4 + count]

        if rectype == 0x00:
            for i, byte in enumerate(payload):
                data[upper + addr + i] = byte
        elif rectype == 0x04:
            upper = int.from_bytes(payload, "big") << 16
        elif rectype == 0x02:
            upper = int.from_bytes(payload, "big") << 4
        elif rectype == 0x01:
            break

    if not data:
        die("Intel HEX file contained no data records")

    lo, hi = min(data), max(data)
    out = bytearray(b"\xff" * (hi - lo + 1))
    for addr, byte in data.items():
        out[addr - lo] = byte
    return bytes(out)


def load_image(path):
    """Read a signed image, refusing the artifact that will not match a board."""
    p = Path(path)
    if not p.is_file():
        die(f"no such file: {path}")
    blob = p.read_bytes()

    if blob[:1] == b":":
        return read_ihex(blob.decode("ascii", "replace"))

    sibling = p.with_suffix(".hex")
    if sibling.is_file():
        die(
            f"{p.name} is a raw binary and {sibling.name} sits beside it.\n"
            f"  The build signs the image TWICE, in separate imgtool runs, and ECDSA\n"
            f"  signatures are randomised -- so these two files hold the same code\n"
            f"  under different signatures, differing in their last 64 bytes. Only\n"
            f"  the .hex reaches merged.hex, so only the .hex is what a flashed board\n"
            f"  is running, and a delta must be computed against that.\n"
            f"  Use: {sibling}"
        )
    return blob


def sign(key_path, payload):
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec, utils
    except ImportError:
        die("needs the 'cryptography' module. Fix: pip install ./tools/tinyfota")

    try:
        key = serialization.load_pem_private_key(Path(key_path).read_bytes(), password=None)
    except (OSError, ValueError, TypeError) as err:
        die(f"cannot read the signing key {key_path}: {err}")
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        die(f"{key_path} is not an ECDSA P-256 key")

    r, s = utils.decode_dss_signature(key.sign(payload, ec.ECDSA(hashes.SHA256())))
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def build(args):
    try:
        import detools
    except ImportError:
        die("needs the 'detools' module. Fix: pip install ./tools/tinyfota")

    # Checked BEFORE the patch is created, not when it is used. Creating a patch
    # over a 400 KB image is the slow part, and discovering afterwards that
    # nothing can sign it wastes all of it.
    sign(args.key, b"")

    from_data = load_image(args.from_image)
    to_data = load_image(args.to_image)

    # A patch from an image to ITSELF, which detools will happily produce: the
    # in-place format carries segment and step structure regardless, so the
    # result is several KB of overhead that changes nothing. Signed and named
    # like any other update, it then travels all the way to a board -- which
    # spends a flash erase, a write and a reboot arriving exactly where it
    # started, and reports success.
    #
    # The usual cause is an unchanged build: rebuilding after editing
    # something outside the firmware sources rebuilds nothing, and ninja
    # re-emits a byte-identical image. MEASURED in ultrawidelock on 2026-08-27:
    # 0 bytes differing between the deployed record and a fresh build, and a
    # 7,391 B patch built from it anyway.
    #
    # The SHA-256 TLV is the right comparison rather than the raw bytes,
    # because it is what the board reports and what the filename carries -- two
    # builds of identical code can differ in their randomised signature while
    # remaining the same image, and that is still nothing to ship.
    if image_sha(args.from_image) == image_sha(args.to_image):
        die(
            f"--from and --to are the same image ({image_sha(args.from_image)[:8].hex()}).\n"
            f"  A patch from an image to itself is several KB that changes nothing, and\n"
            f"  a board would erase, write and reboot to arrive where it started.\n"
            f"  Nothing in the firmware changed -- an edit outside the firmware sources\n"
            f"  rebuilds nothing. Change something in the application and rebuild."
        )

    if args.memory_size:
        memory_size = args.memory_size
        staging_size = args.staging_size
    else:
        if not args.build_dir:
            die("pass either --build-dir or both --memory-size and --staging-size")
        memory_size = partition(args.build_dir, "mcuboot_primary")
        staging_size = partition(args.build_dir, "patch_staging")

    if memory_size % SEGMENT_SIZE:
        die(f"slot size {memory_size} is not a multiple of {SEGMENT_SIZE}")
    for name, data in (("--from", from_data), ("--to", to_data)):
        if len(data) > memory_size:
            die(f"{name} is {len(data)} B, larger than the {memory_size} B slot")

    patch_capacity = staging_size - TINYFOTA_PATCH_OFFSET

    fpatch = io.BytesIO()
    detools.create_patch(
        io.BytesIO(from_data),
        io.BytesIO(to_data),
        fpatch,
        patch_type="in-place",
        compression="heatshrink",
        memory_size=memory_size,
        segment_size=SEGMENT_SIZE,
        minimum_shift_size=args.minimum_shift_size,
        heatshrink_window_sz2=HEATSHRINK_WINDOW_SZ2,
        heatshrink_lookahead_sz2=HEATSHRINK_LOOKAHEAD_SZ2,
    )
    patch = fpatch.getvalue()

    if len(patch) > patch_capacity:
        die(
            f"patch is {len(patch)} B but patch_staging holds {patch_capacity} B "
            f"({staging_size} B less the two control pages).\n"
            f"  The two images differ too much for one delta. Either grow "
            f"patch_staging in the partition map, which moves the flash map "
            f"and so cannot be done over the air, or ship the intermediate build.",
            code=EXIT_PATCH_TOO_BIG,
        )

    body = struct.pack(
        HDR_FMT,
        TINYFOTA_MAGIC,
        TINYFOTA_ABI_VERSION,
        0,
        len(patch),
        len(to_data),
        zlib.crc32(patch),
        zlib.crc32(from_data),
        len(from_data),
    )
    header = body + struct.pack("<I", zlib.crc32(body))
    assert len(header) == TINYFOTA_HDR_LEN
    assert len(body) == TINYFOTA_HDR_CRC_LEN

    signature = sign(args.key, header)
    Path(args.out).write_bytes(header + signature + patch)

    ratio = 100.0 * len(patch) / len(to_data)
    print(f"  from      {len(from_data):>9,} B  crc {zlib.crc32(from_data):#010x}")
    print(f"  to        {len(to_data):>9,} B")
    print(f"  patch     {len(patch):>9,} B  {ratio:.2f}% of the image")
    print(f"  capacity  {patch_capacity:>9,} B  {100.0*len(patch)/patch_capacity:.1f}% used")
    print(f"  wrote     {args.out}")


def recovery(args):
    """Emit the whole signed image, plus a sidecar, for MCUboot serial recovery.

    WHAT THIS IS FOR. Everything else this tool produces is a delta, because
    the board has one MCUboot slot and a delta is the only thing that fits beside
    a running image. Serial recovery is the exception: MCUboot is not running
    the application, so the whole slot is free and a whole image can go into it.
    That is the only path on this board that does not need a starting image to
    subtract from -- which makes it the only one that can rescue a board whose
    application no longer boots, and the only one that needs no probe to do it.

    THE INPUT IS THE .hex, AND IT HAS TO BE. The build signs the image twice, in
    separate imgtool runs, and ECDSA signatures are randomised -- so
    zephyr.signed.bin and zephyr.signed.hex hold the same code under different
    signatures and differ in their last 64 bytes. Only the .hex reaches
    merged.hex, so only the .hex is what an SWD-flashed board is running. Both
    would verify and both would boot, but publishing the other one would mean
    the bytes on a recovered board differ from the bytes on a flashed board for
    no reason anybody could later explain. load_image() refuses the .bin outright
    when the .hex is beside it, which is what makes this hard to get wrong.

    The SHA-256 in the sidecar is the TLV, not the hash of the file: it is the
    value the board reports in its image list, and therefore the one the page
    compares against after the reset to decide whether recovery worked.
    """
    blob = load_image(args.image)
    sha = image_sha(args.image)

    major, minor, rev = (int(x) for x in args.version.split("."))

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Named for the image it IS, where a delta is named for the two it sits
    # between. Same reasoning either way: a stale file under a stable name is
    # the failure that costs a flash dump to diagnose.
    bin_name = f"{args.prefix}-{sha[:8].hex()}.bin"
    bin_path = out_dir / bin_name
    bin_path.write_bytes(blob)

    sidecar = {
        "file": bin_name,
        "size": len(blob),
        "sha256": sha.hex(),
        "version": f"{major}.{minor}.{rev}",
        "board": args.board,
        "kind": "recovery",
    }
    side_path = out_dir / f"{bin_path.stem}.recovery.json"
    side_path.write_text(json.dumps(sidecar, indent=2, sort_keys=True) + "\n")

    print(f"  image     {len(blob):>9,} B  v{major}.{minor}.{rev}")
    print(f"  sha256    {sha.hex()}")
    print(f"  wrote     {bin_path}")
    print(f"  wrote     {side_path}")


def wrap(args):
    """Dress a .wdfu as an MCUboot image so a phone's file picker accepts it.

    The result is a WELL-FORMED image, not a decoy: the header sizes describe
    the real payload and the SHA-256 TLV really is the hash of what precedes it,
    so an app that validates finds everything consistent. What it is NOT is
    bootable -- the payload is a delta, not code, and nothing ever asks MCUboot
    to run it. The board recognises the wrapper by its magic and skips it.
    """
    inner = Path(args.patch).read_bytes()
    if len(inner) < TINYFOTA_HDR_LEN + TINYFOTA_SIG_LEN:
        die(f"{args.patch} is too short to be a .wdfu")
    if struct.unpack("<I", inner[:4])[0] != TINYFOTA_MAGIC:
        die(f"{args.patch} is not a .wdfu (no WDFU magic). Did you pass the wrapped file back in?")

    major, minor, rev = (int(x) for x in args.version.split("."))
    # The build number distinguishes one patch from the next in the phone's
    # image list, which otherwise shows the same version for every file. The
    # patch CRC is already the thing `info` prints, so matching them is a check
    # the user can actually perform.
    build_num = zlib.crc32(inner) & 0xFFFFFFFF

    # magic, load_addr, hdr_size, protect_tlv_size, img_size, flags | version | _pad1.
    # The trailing pad is part of the struct, not slack: without it the header is
    # 28 B, every offset after it shifts, and the board reads the wrong sizes.
    header = (
        struct.pack("<IIHHII", MCUBOOT_IMAGE_MAGIC, 0, MCUBOOT_HDR_LEN, 0, len(inner), 0)
        + struct.pack("<BBHI", major, minor, rev, build_num)
        + struct.pack("<I", 0)
    )
    assert len(header) == MCUBOOT_HDR_LEN

    digest = hashlib.sha256(header + inner).digest()
    tlv_total = 4 + 4 + len(digest)
    trailer = (
        struct.pack("<HH", MCUBOOT_TLV_INFO_MAGIC, tlv_total)
        + struct.pack("<BBH", MCUBOOT_TLV_SHA256, 0, len(digest))
        + digest
    )

    out = header + inner + trailer

    # THE NAME CARRIES BOTH HASHES, and that is the point of it.
    #
    # A stable name is what makes a stale file dangerous: the phone keeps last
    # week's copy under the same name as today's, the picker cannot tell them
    # apart, and the board silently applies the wrong one. That happened --
    # a phone uploaded an older patch whose target build had already been
    # overwritten on the host, and identifying what the board was left running
    # took a flash dump over SWD.
    #
    # These are the MCUboot SHA-256 TLVs, the same values the board prints in
    # its image list, so "does this file apply to what I have" is a comparison
    # you can do by eye between a filename and a line of output.
    from_sha = image_sha(args.from_image)
    to_sha = image_sha(args.to_image)
    stem = f"{args.prefix}-{from_sha[:8].hex()}-to-{to_sha[:8].hex()}"

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    bin_name = f"{stem}.bin"
    bin_path = out_dir / bin_name
    bin_path.write_bytes(out)

    # The zip is what nRF Connect SDK ships and what the phone apps expect to
    # see; the manifest mirrors nrf/scripts/bootloader/generate_zip.py. The
    # two sha fields are ours and are not part of that schema -- extra keys are
    # ignored by readers, and they make the archive self-describing once the
    # filename has been lost to a share sheet.
    manifest = {
        "format-version": 1,
        "time": int(time.time()),
        "name": f"{args.prefix} delta update",
        "files": [
            {
                "type": "application",
                "board": args.board,
                "soc": args.soc,
                "load_address": load_address(args),
                "image_index": "0",
                "slot_index_primary": "1",
                "slot_index_secondary": "2",
                "version_MCUBOOT": f"{major}.{minor}.{rev}",
                "size": len(out),
                "file": bin_name,
                "modtime": int(time.time()),
                "from_sha256": from_sha.hex(),
                "to_sha256": to_sha.hex(),
            }
        ],
    }

    zip_path = out_dir / f"{stem}.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=4))
        z.write(bin_path, bin_name)

    print(f"  patch     {len(inner):>9,} B  crc {zlib.crc32(inner):#010x}")
    print(f"  wrapper   {len(out) - len(inner):>9,} B  header {MCUBOOT_HDR_LEN} + TLV {len(trailer)}")
    print(f"  file      {len(out):>9,} B  v{major}.{minor}.{rev}+{build_num}")
    print(f"  applies to  {from_sha[:8].hex()}   produces  {to_sha[:8].hex()}")
    print(f"  wrote     {bin_path}")
    print(f"  wrote     {zip_path}")


def info(args):
    blob = Path(args.patch).read_bytes()
    if len(blob) < TINYFOTA_HDR_LEN + TINYFOTA_SIG_LEN:
        die("too short to be a .wdfu")

    fields = struct.unpack(HDR_FMT, blob[:TINYFOTA_HDR_CRC_LEN])
    (magic, abi, flags, patch_len, to_len, patch_crc, from_crc, from_len) = fields
    stored_crc = struct.unpack("<I", blob[TINYFOTA_HDR_CRC_LEN:TINYFOTA_HDR_LEN])[0]
    patch = blob[TINYFOTA_HDR_LEN + TINYFOTA_SIG_LEN:]

    print(f"  magic       {magic:#010x} {'ok' if magic == TINYFOTA_MAGIC else 'BAD'}")
    print(f"  abi         {abi} {'ok' if abi == TINYFOTA_ABI_VERSION else 'BAD'}")
    # Named, not just printed. This word decides which port can install the
    # file at all, and "0x0001" tells a reader nothing about that.
    kind = ("whole image (two-slot ports; this applier does not install it)"
            if flags & TINYFOTA_FLAG_FULL_IMAGE else "delta")
    print(f"  flags       {flags:#06x}  {kind}")
    print(f"  from        {from_len:,} B  crc {from_crc:#010x}")
    print(f"  to          {to_len:,} B")
    print(f"  patch       {patch_len:,} B  crc {patch_crc:#010x}")
    print(f"  header crc  {stored_crc:#010x} "
          f"{'ok' if stored_crc == zlib.crc32(blob[:TINYFOTA_HDR_CRC_LEN]) else 'BAD'}")
    print(f"  patch crc   {'ok' if patch_crc == zlib.crc32(patch) else 'BAD'}")
    print(f"  patch len   {'ok' if patch_len == len(patch) else 'BAD'}")


def stage(args):
    """Lay a .wdfu out as a raw image of the patch_staging partition.

    For bench work only: it lets a patch be written over SWD and the APPLIER
    tested on its own, with no radio and no receiver in the picture. That
    separation is the point -- when an over-the-air update fails, this says
    which half failed.

        tinyfota stage update.wdfu --out staging.bin
        arm-zephyr-eabi-objcopy -I binary -O ihex \\
            --change-addresses <patch_staging address> staging.bin staging.hex
        nrfjprog --program staging.hex --sectorerase --verify -r
    """
    blob = Path(args.patch).read_bytes()
    header = blob[:TINYFOTA_HDR_LEN]
    patch = blob[TINYFOTA_HDR_LEN + TINYFOTA_SIG_LEN:]

    if struct.unpack("<I", header[:4])[0] != TINYFOTA_MAGIC:
        die(f"{args.patch} is not a .wdfu")

    # Page 0 header, page 1 the step log left ERASED, page 2 onward the patch.
    # The step log must be 0xff: a stale one would make the bootloader believe
    # steps were already applied and skip them.
    image = bytearray(b"\xff" * TINYFOTA_PATCH_OFFSET)
    image[: len(header)] = header
    image += patch
    Path(args.out).write_bytes(image)
    print(f"  wrote {args.out}  ({len(image):,} B, patch at +{TINYFOTA_PATCH_OFFSET})")


def load_address(args):
    """The manifest's load_address: --load-address, else the primary slot's start."""
    if args.load_address is not None:
        return args.load_address
    if args.build_dir:
        return partition_field(args.build_dir, "mcuboot_primary", "address")
    return 0


def add_parsers(sub):
    """Register build, info, stage, recovery and wrap on the `tinyfota` CLI."""
    b = sub.add_parser("build", help="create a signed patch from two signed images")
    b.add_argument("--from", dest="from_image", required=True,
                   help="the signed image the board is running now")
    b.add_argument("--to", dest="to_image", required=True,
                   help="the signed image it should be running after")
    b.add_argument("--out", required=True)
    b.add_argument("--key", required=True,
                   help="signs the header; same key MCUboot verifies images with")
    b.add_argument("--build-dir",
                   help="read the slot and staging sizes from its partitions.yml")
    b.add_argument("--memory-size", type=lambda v: int(v, 0),
                   help="mcuboot_primary size, if not using --build-dir")
    b.add_argument("--staging-size", type=lambda v: int(v, 0),
                   help="patch_staging size, if not using --build-dir")
    b.add_argument("--minimum-shift-size", type=lambda v: int(v, 0), default=2 * SEGMENT_SIZE,
                   help="forward-shift slack detools works in; must stay a "
                        "multiple of the segment size, and 2 segments is its floor")
    b.set_defaults(func=build)

    i = sub.add_parser("info", help="describe a .wdfu and check its own CRCs")
    i.add_argument("patch")
    i.set_defaults(func=info)

    s = sub.add_parser("stage", help="lay a .wdfu out as a patch_staging image, for SWD")
    s.add_argument("patch")
    s.add_argument("--out", required=True)
    s.set_defaults(func=stage)

    r = sub.add_parser("recovery",
                       help="publish the whole signed image, for MCUboot serial recovery")
    r.add_argument("image", help="the SIGNED .hex -- see the docstring, the .bin is not it")
    r.add_argument("--out-dir", required=True, help="where to write the .bin and its sidecar")
    r.add_argument("--version", default="1.0.0", help="major.minor.revision")
    add_naming(r)
    r.set_defaults(func=recovery)

    w = sub.add_parser("wrap", help="dress a .wdfu as an MCUboot image, for phone tooling")
    w.add_argument("patch")
    w.add_argument("--out-dir", required=True, help="where to write the named .bin and .zip")
    w.add_argument("--from-image", required=True, help="signed image the patch applies to")
    w.add_argument("--to-image", required=True, help="signed image the patch produces")
    w.add_argument("--version", default="1.0.0", help="major.minor.revision shown by the app")
    add_naming(w)
    w.add_argument("--soc", default="", help="the SoC, for the zip manifest")
    w.add_argument("--load-address", type=lambda v: int(v, 0), default=None,
                   help="the primary slot's address, for the zip manifest")
    w.add_argument("--build-dir", help="read --load-address from its partitions.yml")
    w.set_defaults(func=wrap)


def add_naming(p):
    """The two names every published file carries."""
    p.add_argument("--prefix", default="tinyfota",
                   help="file-name prefix: <prefix>-<from>-to-<to>.bin, <prefix>-<sha>.bin")
    p.add_argument("--board", default="", help="the board name, for the manifest and sidecar")


assert struct.calcsize(HDR_FMT) == TINYFOTA_HDR_CRC_LEN, "header layout drifted"
