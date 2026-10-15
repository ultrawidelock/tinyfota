"""The `tinyfota` command: build, info, stage, recovery, wrap, push, release."""

import argparse

from . import __version__, patch, release, smp


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="tinyfota",
        description="Signed, power-cut-safe delta updates for single-slot MCUboot boards. "
                    "FORMAT.md in the repository describes every byte this writes.")
    ap.add_argument("--version", action="version", version=f"tinyfota {__version__}")
    sub = ap.add_subparsers(dest="cmd", required=True, metavar="command")
    patch.add_parsers(sub)
    smp.add_parser(sub)
    release.add_parser(sub)
    args = ap.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
