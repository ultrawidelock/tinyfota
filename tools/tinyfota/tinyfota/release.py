"""Build a release: one delta from every base still in the field, the whole
image for serial recovery, and the index the browser page reads.

    tinyfota release --to build/zephyr.signed.hex \\
        --base releases/v0.1.0/zephyr.signed.hex --base releases/v0.2.0/zephyr.signed.hex \\
        --key signing-key.pem --build-dir build --version 0.3.0 --out-dir release/ota

The single MCUboot slot means an update is a delta against the exact bytes on
the board, so building one needs the image that board is running. Keep the
signed .hex of every release you publish: it cannot be rebuilt, because the
image embeds a build timestamp (measured in ultrawidelock on 2026-08-27 at 105
differing bytes of 408,206 between two builds of identical sources). Without
it, no later release can ever build an update for the boards running it.

THE FAN NEVER FAILS SILENTLY, which is the whole design of this file. Every way
the source project's release path once came up empty without saying so is a
separate, loud outcome here:

  - A base whose delta does not fit patch_staging is SKIPPED, by name, and the
    run continues. That pair of images was never going to work, and failing
    would throw away every delta that did fit and the recovery image. The skip
    is listed again at the end, written to skipped-bases.txt, and raised as a
    GitHub Actions warning when run there.
  - Any other failure -- a missing base, an unreadable image, no partition
    map, a bad key, a base identical to the target -- stops the release with
    a non-zero status. Those mean the tooling or its inputs are wrong.
  - No bases at all is a first release: a notice, and the whole image is
    published alone. Bases given and none usable is a warning, because then
    every board in the field is told no update applies.
"""

import argparse
import os
import shutil
from pathlib import Path

from . import index, patch


def _warn(msg):
    """Print a warning, and annotate the run when it is a GitHub Actions job."""
    print(f"  WARNING  {msg}")
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print(f"::warning::{msg}")


def release(args):
    to_image = Path(args.to_image)
    if not to_image.is_file():
        patch.die(f"no such image: {to_image}")
    out_dir = Path(args.out_dir)
    target_dir = out_dir / args.target
    if target_dir.exists():
        shutil.rmtree(target_dir)
    target_dir.mkdir(parents=True)

    # Checked before any delta is built, as `build` does: discovering after the
    # slow part that nothing can sign is wasted work.
    patch.sign(args.key, b"")

    bases = list(args.base or [])
    if args.bases_dir:
        bases += sorted(str(p) for p in Path(args.bases_dir).rglob("*.hex"))
    # Sorted and de-duplicated, so a re-run fans out in the same order: each
    # delta is independent of every other.
    bases = sorted(dict.fromkeys(str(Path(b)) for b in bases))
    for base in bases:
        if not Path(base).is_file():
            patch.die(f"no such base image: {base}")

    if not bases:
        print("  NOTICE   no base images given, so there is nothing to build a DELTA from.\n"
              "           On a first release that is correct -- no board runs an older\n"
              "           image yet. The whole image is still published for serial\n"
              "           recovery, so the release has an install path.")

    skipped = []
    built = 0
    fan = out_dir / "fan.wdfu"
    for base in bases:
        print(f"  delta from {base}")
        ns = argparse.Namespace(
            from_image=base, to_image=str(to_image), out=str(fan), key=args.key,
            build_dir=args.build_dir, memory_size=args.memory_size,
            staging_size=args.staging_size, minimum_shift_size=args.minimum_shift_size)
        try:
            patch.build(ns)
        except patch.PatchTooBig:
            print(f"  SKIPPED    {base}")
            skipped.append(base)
            continue
        wrap_ns = argparse.Namespace(
            patch=str(fan), out_dir=str(target_dir), from_image=base, to_image=str(to_image),
            version=args.version, prefix=args.prefix, board=args.board, soc=args.soc,
            load_address=args.load_address, build_dir=args.build_dir)
        patch.wrap(wrap_ns)
        built += 1
    if fan.exists():
        fan.unlink()

    print("  whole image for serial recovery")
    patch.recovery(argparse.Namespace(image=str(to_image), out_dir=str(target_dir),
                                      version=args.version, prefix=args.prefix,
                                      board=args.board))

    index.write_index(out_dir / "ota-index.json", target_dir, args.version, args.target,
                      args.name)

    # LAST, not beside the skip, because a line printed in the middle of a fan
    # scrolls past and the release still looks clean. What was skipped decides
    # whether anyone has to be told to plug a cable in.
    skipped_txt = out_dir / "skipped-bases.txt"
    if skipped:
        skipped_txt.write_text("".join(f"{b}\n" for b in skipped))
        print(f"\n  {len(skipped)} base image(s) got NO delta:")
        for base in skipped:
            print(f"    {base}")
        print("  Boards running those images have no over-the-air path from this\n"
              "  release. The page reads the index, finds no entry for their hash\n"
              "  and offers the whole-image reinstall over serial recovery instead.\n"
              "  To give them one, ship an intermediate build or grow patch_staging --\n"
              "  which moves the flash map, so it cannot be done over the air either.\n")
        for base in skipped:
            _warn(f"no delta from {base}: it differs from this release by more than "
                  f"patch_staging holds, so boards running it get no over-the-air "
                  f"update and will be offered the whole-image reinstall")
    elif skipped_txt.exists():
        skipped_txt.unlink()
    if bases and built == 0:
        _warn(f"{len(bases)} base image(s) were given and none yielded a delta, so this "
              f"release ships NO deltas and every board in the field will be told no "
              f"update applies")
    print(f"  release: {built} delta(s), {len(skipped)} skipped, 1 recovery image")


def add_parser(sub):
    r = sub.add_parser("release",
                       help="deltas from every base in the field, the recovery image, "
                            "and the index")
    r.add_argument("--to", dest="to_image", required=True,
                   help="this release's signed image (the .hex)")
    r.add_argument("--base", action="append",
                   help="a signed .hex still in the field; repeat for each")
    r.add_argument("--bases-dir", help="also take every *.hex under this directory")
    r.add_argument("--key", required=True, help="the image-signing key")
    r.add_argument("--out-dir", required=True,
                   help="writes <target>/ with the files, ota-index.json, and "
                        "skipped-bases.txt when any base was skipped")
    r.add_argument("--version", default="1.0.0", help="major.minor.revision")
    r.add_argument("--target", default="default", help="the index key for this board")
    r.add_argument("--name", default="", help="the board name the page shows")
    r.add_argument("--build-dir",
                   help="read the slot and staging sizes from its partition map")
    r.add_argument("--memory-size", type=lambda v: int(v, 0),
                   help="mcuboot_primary size, if not using --build-dir")
    r.add_argument("--staging-size", type=lambda v: int(v, 0),
                   help="patch_staging size, if not using --build-dir")
    r.add_argument("--minimum-shift-size", type=lambda v: int(v, 0),
                   default=2 * patch.SEGMENT_SIZE,
                   help="forward-shift slack detools works in")
    patch.add_naming(r)
    r.add_argument("--soc", default="", help="the SoC, for the zip manifest")
    r.add_argument("--load-address", type=lambda v: int(v, 0), default=None,
                   help="the primary slot's address, for the zip manifest")
    r.set_defaults(func=release)
