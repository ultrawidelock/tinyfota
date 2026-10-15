<!-- DISCLAIMER: maintainer adds this -->

# tinyfota

Signed, power-cut-safe delta firmware updates for chips that cannot hold two
images. The patch is applied in place, from inside unmodified MCUboot, and
delivered from a phone app, a browser or a cable.

Extracted from ultrawidelock@d6e23761.

## The problem

An nRF52833 has 512 KB of flash and no external flash. The sample's
application slot is 424 KB, and two of those do not fit, so dual-slot MCUboot
is out, and with it the usual way to update over the air. tinyfota is the way that fits:

- the application receives a **delta** (a few KB for a small change), checks
  its signature and stages it in a 32 KB partition;
- on the next boot, a hook inside MCUboot rewrites the one application slot
  **in place**, recording progress as it goes so a power cut resumes instead
  of bricking;
- MCUboot then validates the result's signature as it always does, and drops
  into serial recovery if it fails.

It is for anyone shipping Zephyr firmware on a chip whose application is more
than about half its flash, who wants updates over Bluetooth without adding a
flash chip.

## Quick start

You need an NCS v3.3.0 workspace (`nrfutil sdk-manager install v3.3.0`),
Python 3.9 or newer, and a C and C++ compiler (`tools/tinyfota/README.md`
says exactly what).

```
# 1. the host tool
python3 -m venv .venv && . .venv/bin/activate && pip install ./tools/tinyfota

# 2. from the NCS workspace: build and flash the sample, and keep this build
west build -b nrf52833dk/nrf52833 --sysbuild -d build/v1 <tinyfota>/samples/nrf52833
west flash -d build/v1

# 3. change something in the sample, then build into another directory
west build -b nrf52833dk/nrf52833 --sysbuild -d build/v2 <tinyfota>/samples/nrf52833

# 4. make the delta and push it; press sw0 on the board when asked
tinyfota build --from build/v1/nrf52833/zephyr/zephyr.signed.hex \
    --to build/v2/nrf52833/zephyr/zephyr.signed.hex \
    --build-dir build/v2 --key <signing key .pem> --out update.wdfu
tinyfota push update.wdfu
```

The sample signs with MCUboot's published development key, which is
`<ncs>/bootloader/mcuboot/root-ec-p256.pem`, unless you set
`SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`. Fine for a desk, never for a product
               . Keep the `.hex` of every build you flash or release: it is
the only input a later delta can be built from.

## What works

| Path | How | Transport |
|---|---|---|
| Phone app | `tinyfota wrap` dresses the `.wdfu` as an MCUboot image; open its output in nRF Device Manager | SMP over Bluetooth |
| Browser | serve `web/` (for example `python3 -m http.server -d web 8000`) and open `http://localhost:8000` in Chrome or Edge | Web Bluetooth or Web Serial |
| CLI | `tinyfota push`, which finds the board by service UUID | SMP over Bluetooth, or serial with `--serial` |

The board reports its running image's SHA-256 at every boot, which is what an
updater reads back to confirm. All three are checked on the host here: `make wire` holds the
browser's bytes to the CLI's, and the flow check drives the page. The source
project proved the browser path on hardware (a delta applied over Web
Bluetooth from Chrome). tinyfota itself has not run on hardware yet.

## Sizes

Measured on 2026-10-16 with NCS v3.3.0 (Zephyr 4.3.99), building
`samples/nrf52833` for both boards. Byte counts are `zephyr.bin` sizes and
linker totals, not estimates.

| What | nRF52833 DK | DWM3001CDK |
|---|---|---|
| Applier inside MCUboot | 3,776 B flash, 1,072 B RAM | not built separately |
| MCUboot total, against its partition | 33,888 B of 40,960 B (82.7%) | 33,644 B of 40,960 B (82.1%) |
| Staging partition | 32,768 B, of which 24,576 B is patch | same |
| Sample application, linked | 190,608 B of 433,664 B | 190,284 B of 433,664 B |
| Patch between two adjacent sample builds | 3,645 B (1.91% of the image) | not built |

- **Applier**: MCUboot built with and without `CONFIG_TINYFOTA_APPLIER`
  (30,112 B without). The linker map agrees: 3,766 B from the tinyfota library
  (detools 1,880 B, applier 1,076 B, heatshrink 574 B, flash port 192 B, CRC
  44 B), plus the 8 B `SYS_INIT` entry and 2 B of alignment. The 1,072 B of
  RAM is the applier's static state.
- **Adjacent builds**: image version 0.1.0, then 0.1.1 with one constant changed
  (`CONFIG_TINYFOTA_WINDOW_MS`, 300000 to 240000), which leaves 11 B of the
  unsigned image different. The patch carries that, the new header and the new
  signature TLVs.

## Limits

| Limit | Why | Lifted in |
|---|---|---|
| Needs small, uniform erase pages | The format fixes the patch segment at 4,096 B, so every page in both partitions must be one size that divides 4,096 B | |
| Write size is fixed at 4 B | The applier combines writes into 4 B words. Flash that writes 8 B, 16 B or 32 B, or error-correcting flash that cannot rewrite a word, is refused when an area is opened | v0.2 |
| No rollback | Patching in place destroys the old image as it goes. If the result does not validate, MCUboot serial recovery and a cable are the way back | v0.3 |
| MCUboot on Zephyr only | The applier runs from a Zephyr `SYS_INIT` hook ahead of MCUboot's `main()` | v0.4 |
| A delta must fit the staging partition | 24,576 B of patch in the sample layout. `tinyfota release` skips a base whose delta does not fit, and says so | |

## Proof

- **Power cuts.** `make powercut` runs a real update between two sample
  builds through the real receiver, applier and detools on a strict NOR flash
  model, and cuts the power at every one of its 3,263 flash operations, once
  losing the operation and once tearing it halfway: 6,526 runs. Every run ends on
  the target image or on the untouched base. It found two applier defects,
  both fixed (see the P6 commit).
- **Fuzzing.** `make fuzz` builds libFuzzer targets for the header parsers,
  the receiver's frame and SMP upload paths, and patch application. Ten-minute
  runs found two crashes in detools on a corrupt patch. Both are fixed in the
  vendored copy and sent upstream (`THIRD_PARTY.md`), and all 372 saved crash
  inputs now run clean. A second ten-minute run per target over the finished
  tree found no crash. It saved two slow apply inputs, one past the 10 s
  timeout while four targets shared the machine. Run alone, they finish in
  8.0 s and 11.9 s under the sanitizers, and both need a step log that only
  the fuzzer can forge. No fuzz crash is open.

## Roadmap

| Version | Scope |
|---|---|
| v0.2 | Configurable write size (8 B, 16 B, 32 B; error-correcting flash that cannot rewrite a word), proven on `<SECOND_BOARD>` |
| v0.3 | Rollback without a second slot: ship a reverse patch with the forward one, boot the new image in test mode, and apply the reverse patch if the app never confirms or the watchdog fires |
| v0.4 | Integrations for other bootloaders that have a pre-boot hook. tinyfota is never a bootloader itself: MCUboot or the host bootloader keeps boot, signature checks and recovery. |

## Build and test

```
make test       # host unit tests
make asan       # the same tests under AddressSanitizer and UndefinedBehaviorSanitizer
make seams      # the symbols the core expects a port to provide
make powercut   # the power-cut simulator
make wire       # browser and CLI produce the same bytes (needs python3 and node)
```

On a machine where `cc` or `nm` is not the tool you want, pass both:
`make test CC=/usr/bin/clang NM="xcrun nm"`.

## Documents

| File | What it holds |
|---|---|
| `FORMAT.md` | every byte of the update file and the staging partition |
| `PORTING.md` | the seams a port provides, and what a new chip must satisfy |
| `THIRD_PARTY.md` | detools, heatshrink and sais, their licenses, and the local detools patches |

## License

ISC, see `LICENSE`. Third-party code is listed in `THIRD_PARTY.md`.
