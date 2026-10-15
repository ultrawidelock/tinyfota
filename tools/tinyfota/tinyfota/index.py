"""Write the index the browser update page looks a board up in.

    tinyfota release ...          writes it as the last step of a release
    python -m tinyfota.index --self-test

ONE SHAPE PER TARGET: "updates", a map KEYED BY THE SHA-256 THE BOARD REPORTS.
One MCUboot slot and a small patch_staging partition mean what travels is a
delta against the exact bytes already on the part, and a delta is valid for
exactly one starting image. So shipping an update means shipping one file per
release anyone might still be running -- the fan -- and the page picks from it
by reading one 32-byte value off the board. No version parsing, no ordering, no
"is 0.3.1 newer than 0.3.10".

A board whose hash is not in the map has NO over-the-air path. That is the
single-slot design, not a bug to route around, and the page says so and offers
the whole-image reinstall over serial recovery.

Nothing here is recomputed. It reads the manifest `tinyfota wrap` writes into
each .zip and the sidecar `tinyfota recovery` writes beside the whole image --
the run that signed a file is the only thing that knows what went into it.
"""

import datetime
import json
import sys
import zipfile
from pathlib import Path

SCHEMA = 1


def die(msg):
    sys.stdout.flush()
    sys.exit(f"tinyfota index: {msg}")


def read_wrap_manifest(zip_path):
    """Pull the one file entry out of a `wrap`-built .zip.

    The schema mirrors nrf/scripts/bootloader/generate_zip.py; from_sha256 and
    to_sha256 are tinyfota's own additions to it.
    """
    try:
        with zipfile.ZipFile(zip_path) as z:
            manifest = json.loads(z.read("manifest.json"))
    except (OSError, KeyError, zipfile.BadZipFile, json.JSONDecodeError) as err:
        die(f"{zip_path}: not a wrap-built archive ({err})")

    files = manifest.get("files") or []
    if len(files) != 1:
        die(f"{zip_path}: expected exactly one file entry, found {len(files)}")
    entry = files[0]

    for key in ("from_sha256", "to_sha256", "size", "file"):
        if key not in entry:
            die(f"{zip_path}: manifest has no {key}. Built by an older wrap?")
    return entry


def collect_deltas(deltas_dir):
    """Every delta in a directory, keyed by the image each one applies to."""
    updates = {}
    tos = set()

    for zip_path in sorted(Path(deltas_dir).glob("*.zip")):
        entry = read_wrap_manifest(zip_path)
        from_sha = entry["from_sha256"].lower()
        to_sha = entry["to_sha256"].lower()

        bin_path = Path(deltas_dir) / entry["file"]
        if not bin_path.is_file():
            die(f"{zip_path} names {entry['file']}, which is not beside it")

        # An update from an image to ITSELF. `tinyfota build` refuses to
        # build one now, but this is the file that decides what gets PUBLISHED,
        # and an older delta could still be sitting in the directory. Left in,
        # the page would offer a board an update to the image it is already
        # running -- and then "verify" it by checking the hash still matches,
        # which it trivially does. Success, reported for nothing.
        if from_sha == to_sha:
            die(
                f"{entry['file']} applies to the image it produces ({from_sha[:16]}...).\n"
                f"  That is an update to the image the board already runs. It usually\n"
                f"  means the build did not change between the two releases."
            )

        # Two deltas claiming the same starting image cannot both be right, and
        # picking one silently would mean the page installs a different build
        # than the directory listing suggests. Refuse instead.
        if from_sha in updates:
            die(
                f"two updates apply to {from_sha[:16]}...:\n"
                f"  {updates[from_sha]['file']}\n  {entry['file']}\n"
                f"  A stale file from an earlier build is the usual cause."
            )

        updates[from_sha] = {
            "file": entry["file"],
            "size": entry["size"],
            "to": to_sha,
            "version": entry.get("version_MCUBOOT", "unknown"),
        }
        tos.add(to_sha)

    if len(tos) > 1:
        die(
            "the deltas do not all produce the same image:\n  "
            + "\n  ".join(sorted(tos))
            + "\n  A fan has to converge, or the page cannot say what it is installing."
        )

    return updates, (tos.pop() if tos else None)


def self_test():
    """Prove the index means what the page assumes it means.

    The page trusts three things this file is the only guard for: that a hash
    maps to exactly one update, that every delta in a release produces the SAME
    image, and that a delta whose .bin is missing is caught here rather than as
    a 404 halfway through someone's update. All three are silent failures in
    the field, so they are pinned here.
    """
    import subprocess                                          # noqa: PLC0415
    import tempfile                                            # noqa: PLC0415

    fails = 0
    total = 0

    def wrap_zip(directory, from_sha, to_sha, version="0.3.1", write_bin=True):
        stem = f"tinyfota-{from_sha[:8]}-to-{to_sha[:8]}"
        payload = b"\x00" * 64
        if write_bin:
            (Path(directory) / f"{stem}.bin").write_bytes(payload)
        manifest = {
            "format-version": 1,
            "files": [{
                "file": f"{stem}.bin",
                "size": len(payload),
                "version_MCUBOOT": version,
                "from_sha256": from_sha,
                "to_sha256": to_sha,
            }],
        }
        with zipfile.ZipFile(Path(directory) / f"{stem}.zip", "w") as z:
            z.writestr("manifest.json", json.dumps(manifest))
        return stem

    def run(directory, out):
        return subprocess.run(
            [sys.executable, "-m", "tinyfota.index", "--out", out, "--deltas-dir", directory,
             "--version", "v0.3.1"],
            capture_output=True, text=True, check=False)

    def check(name, ok, detail=""):
        nonlocal fails, total
        total += 1
        if ok:
            print(f"  ok   {name}")
        else:
            fails += 1
            print(f"  FAIL {name}  {detail}")

    def recovery_files(directory, sha, size=4096, version="0.3.1", claim=None):
        """A recovery .bin and its sidecar, as `tinyfota recovery` would have
        written them. `claim` lets a test lie about the size."""
        name = f"tinyfota-{sha[:16]}.bin"
        (Path(directory) / name).write_bytes(b"\x00" * size)
        (Path(directory) / f"tinyfota-{sha[:16]}.recovery.json").write_text(
            json.dumps({
                "file": name,
                "size": claim if claim is not None else size,
                "sha256": sha,
                "version": version,
                "board": "test",
                "kind": "recovery",
            })
        )

    a, b, c = "aa" * 32, "bb" * 32, "cc" * 32

    # 1. The happy path: two starting images, one destination.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, c)
        wrap_zip(d, b, c)
        out = str(Path(d) / "index.json")
        rc = run(d, out)
        check("two deltas index cleanly", rc.returncode == 0, rc.stderr.strip())
        if rc.returncode == 0:
            idx = json.loads(Path(out).read_text())
            t = idx["targets"]["default"]
            check("keyed by the starting hash", set(t["updates"]) == {a, b})
            check("latest is the shared destination", t["latest"]["sha256"] == c)
            check("transport is smp", t["transport"] == "smp")
            check("schema is stamped", idx["schema"] == SCHEMA)
            check("version carries through", t["updates"][a]["version"] == "0.3.1")

    # 2. Two updates claiming the same board. Picking one silently would mean
    #    the page installs something other than what the directory implies.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, b)
        wrap_zip(d, a, c)
        rc = run(d, str(Path(d) / "index.json"))
        check("duplicate starting image is refused", rc.returncode != 0)
        check("and says which two files", "two updates apply to" in rc.stderr)

    # 3. A fan that does not converge. Two destinations means the page cannot
    #    say what it is about to install.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, b)
        wrap_zip(d, c, a)
        rc = run(d, str(Path(d) / "index.json"))
        check("divergent destinations refused", rc.returncode != 0)
        check("and says so", "do not all produce" in rc.stderr)

    # 4. A manifest whose .bin was never written.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, c, write_bin=False)
        rc = run(d, str(Path(d) / "index.json"))
        check("missing payload refused", rc.returncode != 0)
        check("and names the file", "which is not beside it" in rc.stderr)

    # 5. An empty directory is an error, not an empty index: an empty index
    #    published to the site would read as "no updates exist" forever.
    with tempfile.TemporaryDirectory() as d:
        rc = run(d, str(Path(d) / "index.json"))
        check("empty directory refused", rc.returncode != 0)

    # 9. A recovery image beside the deltas: the whole-image path, for a board
    #    whose application does not run and therefore cannot be asked what it is.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, c)
        recovery_files(d, c, size=406524)
        out = str(Path(d) / "index.json")
        rc = run(d, out)
        check("a recovery image indexes cleanly", rc.returncode == 0, rc.stderr.strip())
        if rc.returncode == 0:
            t = json.loads(Path(out).read_text())["targets"]["default"]
            check("recovery is published", "recovery" in t)
            check("recovery carries the size", t["recovery"]["size"] == 406524)
            check("recovery is the image the deltas produce",
                  t["recovery"]["sha256"] == c)
            check("the deltas are still there beside it", set(t["updates"]) == {a})

    # 10. A build with no recovery image is still a valid index. The page just
    #     does not offer the option, which is the whole of the degradation.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, c)
        out = str(Path(d) / "index.json")
        rc = run(d, out)
        check("no recovery image is not an error", rc.returncode == 0)
        if rc.returncode == 0:
            t = json.loads(Path(out).read_text())["targets"]["default"]
            check("and no recovery key is emitted", "recovery" not in t)

    # 11. THE ONE THAT MATTERS. A recovery image that is not what the deltas
    #     produce would leave a rescued board on a build nothing updates.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, c)
        recovery_files(d, b)
        rc = run(d, str(Path(d) / "index.json"))
        check("a recovery image that is not the latest is refused",
              rc.returncode != 0)
        check("and says what it would have left behind",
              "no update applies to" in rc.stderr, rc.stderr.strip())

    # 12. Two recovery images: one of them is stale, and nothing here can tell
    #     which, so neither gets published.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, c)
        recovery_files(d, c)
        recovery_files(d, b)
        rc = run(d, str(Path(d) / "index.json"))
        check("two recovery images are refused", rc.returncode != 0)
        check("and both are named", "more than one recovery image" in rc.stderr)

    # 13. A sidecar whose size does not match the file beside it. Same class of
    #     bug as the delta size check, and the same reason to refuse.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, c)
        recovery_files(d, c, size=4096, claim=9999)
        rc = run(d, str(Path(d) / "index.json"))
        check("a recovery size mismatch is refused", rc.returncode != 0)
        check("and says both numbers",
              "4,096" in rc.stderr and "9,999" in rc.stderr, rc.stderr.strip())

    # 14. THE FIRST RELEASE. No board is running an older image, so there is
    #     nothing to compute a delta against -- but the whole image is still
    #     installable through serial recovery, and it is the only over-the-air
    #     artifact this release can have. Refusing it would mean a brand new
    #     release ships no update path at all.
    with tempfile.TemporaryDirectory() as d:
        recovery_files(d, c, size=406524)
        out = str(Path(d) / "index.json")
        rc = run(d, out)
        check("a recovery image with no deltas is a valid index",
              rc.returncode == 0, rc.stderr.strip())
        if rc.returncode == 0:
            t = json.loads(Path(out).read_text())["targets"]["default"]
            check("first release: the whole image defines the latest",
                  t["latest"]["sha256"] == c)
            check("first release: the fan is empty rather than absent",
                  t["updates"] == {})
            check("first release: recovery is published", t["recovery"]["sha256"] == c)

    # 15. Neither deltas nor a whole image is still nothing to publish.
    with tempfile.TemporaryDirectory() as d:
        rc = run(d, str(Path(d) / "index.json"))
        check("an empty directory is still refused", rc.returncode != 0)
        check("and says both things are missing",
              "no recovery image" in rc.stderr, rc.stderr.strip())

    # The per-check rows above are for a CI log; this line is for a person
    # reading the output directly. Counted rather than hardcoded, so
    # it cannot drift away from the number of checks actually run -- the first
    # draft of this file said 15 while running 13.
    if fails:
        print(f"\n  {fails} of {total} checks FAILED")
        return 1
    print(f"\nRESULT: PASS ({total} checks)")
    return 0


def collect_recovery(deltas_dir, latest):
    """The whole-image entry, if one was published beside the deltas.

    WHY THIS IS SEPARATE FROM THE DELTAS. Every entry in `updates` answers "the
    board is running X, what applies to it"; this one answers "the board is
    running nothing I recognise, or nothing at all". It is reached through
    MCUboot's serial recovery rather than through the application, so it has no
    starting hash, no window gate and no delta -- and it is the only path on
    a single-slot board that can rescue an image that does not boot.

    Optional on purpose. A build that did not publish one still produces a valid
    index; the page simply does not offer the recovery option.
    """
    sidecars = sorted(Path(deltas_dir).glob("*.recovery.json"))
    if not sidecars:
        return None

    if len(sidecars) > 1:
        die(
            "more than one recovery image in "
            f"{deltas_dir}:\n  " + "\n  ".join(x.name for x in sidecars)
            + "\n  A stale file from an earlier build is the usual cause."
        )

    meta = json.loads(sidecars[0].read_text())
    for key in ("file", "size", "sha256", "version"):
        if key not in meta:
            die(f"{sidecars[0].name} has no {key}. Built by an older recovery step?")

    blob = Path(deltas_dir) / meta["file"]
    if not blob.is_file():
        die(f"{sidecars[0].name} names {meta['file']}, which is not beside it")

    actual = blob.stat().st_size
    if actual != meta["size"]:
        die(f"{meta['file']} is {actual:,} B, but its sidecar says {meta['size']:,} B")

    # THE RECOVERY IMAGE MUST BE THE IMAGE THE DELTAS CONVERGE ON, and this is
    # the guard in this file that matters most.
    #
    # A recovery image is what someone reaches for when a board is already in
    # trouble. If it installs a DIFFERENT build than the over-the-air path
    # produces, the board comes back running something no published delta
    # applies to -- so the next update finds no path, the page correctly says
    # so, and the owner is left with a board that cannot be updated at all by
    # the route that was supposed to rescue it. Refusing here costs a release;
    # not refusing costs a field visit.
    # With no deltas there is nothing to be consistent WITH, and the whole image
    # is then the definition of the current release rather than a claim about it.
    if latest is not None and meta["sha256"].lower() != latest.lower():
        die(
            f"the recovery image is not the image the deltas produce:\n"
            f"  recovery  {meta['sha256'][:16]}...  ({meta['file']})\n"
            f"  deltas    {latest[:16]}...\n"
            f"  Recovering a board would leave it on a build no update applies to."
        )

    return {
        "file": meta["file"],
        "size": meta["size"],
        "sha256": meta["sha256"].lower(),
        "version": meta["version"],
    }


def write_index(out, deltas_dir, version="", target="default", name=""):
    """Index one target's deltas and recovery image. Returns the index dict."""
    updates, latest = collect_deltas(deltas_dir)
    rec = collect_recovery(deltas_dir, latest)

    # A FAN OF NO DELTAS IS VALID WHEN A WHOLE IMAGE IS PUBLISHED, and the
    # first release is exactly that case: no board in the world is running
    # an older image, so there is nothing to compute a delta against -- but
    # the whole image is still installable, over MCUboot serial recovery,
    # and it is the only over-the-air artifact that release can have. The
    # page then offers the reinstall option and no others, which is honest
    # rather than degraded.
    if not updates and not rec:
        die(f"no *.zip deltas and no recovery image in {deltas_dir}")

    entry = {
        "transport": "smp",
        "name": name or target,
        "dir": Path(deltas_dir).name,
        "latest": {
            "version": version or "unknown",
            # With no deltas there is no shared destination to read it from,
            # so the whole image says what the current release is.
            "sha256": latest or rec["sha256"],
        },
        "updates": updates,
    }
    if rec:
        entry["recovery"] = rec

    index = {
        "schema": SCHEMA,
        "generated": datetime.datetime.now(datetime.timezone.utc)
                     .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "targets": {target: entry},
    }

    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(index, indent=2, sort_keys=True) + "\n")

    n = len(entry["updates"])
    print(f"  {target:<14} {n} update{'' if n == 1 else 's'} "
          f"-> {entry['latest']['sha256'][:16]}...")
    if "recovery" in entry:
        print(f"  {'':<14} + whole image, {entry['recovery']['size']:,} B "
              f"for serial recovery")
    print(f"  wrote     {out}")
    return index


def main():
    """`python -m tinyfota.index`: index a directory, or prove the guarantees."""
    import argparse                                            # noqa: PLC0415

    if "--self-test" in sys.argv:
        sys.exit(self_test())

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true",
                    help="prove the index's own guarantees, then exit")
    ap.add_argument("--out", required=True, help="where to write ota-index.json")
    ap.add_argument("--deltas-dir", required=True,
                    help="directory of wrap-built deltas (.bin beside .zip)")
    ap.add_argument("--version", default="", help="release version these belong to")
    ap.add_argument("--target", default="default", help="the index key for this board")
    ap.add_argument("--name", default="", help="the board name the page shows")
    args = ap.parse_args()
    write_index(args.out, args.deltas_dir, args.version, args.target, args.name)


if __name__ == "__main__":
    main()
