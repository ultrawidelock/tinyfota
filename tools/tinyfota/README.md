# tinyfota, the host tool

One command, `tinyfota`, for everything that happens off the board:

| Command | What it does |
|---|---|
| `build` | signed delta from two signed MCUboot images; refuses an image to itself |
| `info` | describes a `.wdfu` and checks its CRCs |
| `stage` | lays a `.wdfu` out as a raw `patch_staging` image, for writing over SWD |
| `recovery` | publishes the whole signed image and a sidecar, for MCUboot serial recovery |
| `wrap` | dresses a `.wdfu` as an MCUboot image so phone apps accept it |
| `push` | pushes a patch over SMP, by Bluetooth or a serial port, as a phone would |
| `release` | deltas from N past bases, the recovery image, and the browser page's index |

`FORMAT.md` at the repository root describes every byte these write.

## What a fresh machine needs

Checked on PyPI on 2026-10-15: **detools publishes no wheel for any release**,
on any platform, only a source distribution. Installing it compiles three
extensions, and one of them (HDiffPatch) is C++. Every other dependency
(detools' own, plus cryptography, bleak and pyserial) ships wheels for macOS
arm64 and Linux x86_64 up to CPython 3.14, or is pure Python.

So before `pip install` you need:

| | macOS arm64 | Linux x86_64 |
|---|---|---|
| Python | 3.9 or newer, with `venv` | 3.9 or newer, with `venv` |
| C and C++ compilers | Xcode Command Line Tools: `xcode-select --install` | `gcc` and `g++` |
| Python headers | come with the python.org or Homebrew Python | `python3-dev` (Debian, Ubuntu) |

On Debian or Ubuntu that is
`sudo apt-get install python3-venv python3-dev gcc g++`. A missing `g++` fails
late, at the last of the three extensions; the source project lost a whole
release job to exactly that. The macOS column was verified here, on macOS 26 with
Python 3.14 (`pip install` in a new virtualenv). The Linux column was not run
here.

Then:

```
python3 -m venv .venv
.venv/bin/pip install ./tools/tinyfota
.venv/bin/tinyfota --help
```

If `cc` or `c++` on your PATH is not a working compiler, point pip at one:
`CC=/usr/bin/clang CXX=/usr/bin/clang++ .venv/bin/pip install ./tools/tinyfota`.

## Finding a board

`push` finds a board by the SMP service UUID it advertises, never by name: a
31-byte advertisement truncates names, and a name filter then fails silently.
When more than one board advertises the service, `push` lists them and asks for
`--address`.

## Releases

`release` builds one delta per base image still in the field. A delta that does
not fit the staging partition skips its base, says so by name at the end of the
run, writes `skipped-bases.txt`, and raises a GitHub Actions warning when run
there. Any other failure stops the release with a non-zero status. Keep the
signed `.hex` of every release you publish: it is the only input a later delta
can be built from.
