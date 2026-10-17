# tinyfota

Firmware updates over Bluetooth for chips too small to hold two copies of
their firmware. Signed. Survives power cuts. Sends only what changed.

Extracted from ultrawidelock@d6e23761.

**Status:** passes every host test (unit, sanitizers, power-cut, fuzz) and
builds on upstream Zephyr v4.4.2 and NCS v3.3.0. Not yet run on a board; the
source project proved the browser path on hardware.

**Disclaimer:** experimental software, provided as is with no warranty of any
kind (`LICENSE`). Test every update path on your own boards before you ship it.
Not affiliated with or endorsed by Nordic Semiconductor, the Zephyr Project or
MCUboot; their names here only say what tinyfota works with.

## How it works

```
  old.hex + new.hex
         |
         |  tinyfota build
         v
  update.wdfu    only the changes, signed, a few KB
         |
         |  phone, browser or cable
         v
  +----------------------------- board ------------------------------+
  |  1. app checks the signature, parks the update in staging        |
  |  2. reboot                                                       |
  |  3. bootloader rewrites the app in place, logging each step      |
  |     power cut? it resumes on next boot instead of bricking       |
  |  4. bootloader checks the new app's signature, boots it          |
  +------------------------------------------------------------------+
```

The bootloader is unmodified MCUboot. tinyfota is a small hook inside it, never
a replacement.

## Why

A normal update needs room for two full copies. This chip has room for one.

```
  nRF52833: 512 KB flash, no external flash

  0 KB                                                          512 KB
  +---------+-------------------------------------+---------+--------+
  | MCUboot | app                                 | staging | config |
  |  40 KB  | 424 KB, rewritten in place          |  32 KB  |  16 KB |
  +---------+-------------------------------------+---------+--------+
```

|                    | Two-slot update  | tinyfota            |
|--------------------|------------------|---------------------|
| Flash needed       | 2 x app          | 1 x app + 32 KB     |
| Download           | whole app        | changes only        |
| Power cut mid-way  | safe             | safe, resumes       |
| Undo a bad update  | yes (swap mode)  | not yet, see Limits |

## Quick start

Needs one SDK from the table, Python 3.12+ and a C and C++ compiler
(`tools/tinyfota/README.md` has the details). Upstream Zephyr needs nothing
from Nordic.

| SDK | Get it |
|---|---|
| Upstream Zephyr v4.4.2 | step 0 below |
| NCS v3.3.0 | `nrfutil sdk-manager install v3.3.0`, then skip step 0 |

```sh
# 0. upstream Zephyr: workspace, Python packages, toolchain (in a venv)
pip install west
west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.4.2 zephyrproject
cd zephyrproject && west update && west packages pip --install && west sdk install

# 1. install the host tool
python3 -m venv .venv && . .venv/bin/activate && pip install ./tools/tinyfota

# 2. from the workspace root: build and flash v1, keep the build
west build -b nrf52833dk/nrf52833 --sysbuild -d build/v1 <tinyfota>/samples/nrf52833
west flash -d build/v1

# 3. change something in the sample, build v2
west build -b nrf52833dk/nrf52833 --sysbuild -d build/v2 <tinyfota>/samples/nrf52833

# 4. make the update and send it; press sw0 on the board when asked
tinyfota build --from build/v1/nrf52833/zephyr/zephyr.signed.hex \
    --to build/v2/nrf52833/zephyr/zephyr.signed.hex \
    --build-dir build/v2 --key <workspace>/bootloader/mcuboot/root-ec-p256.pem \
    --out update.wdfu
tinyfota push update.wdfu
```

- **Keep the `.hex` of every build you ship.** It is the only thing a later
  update can be built from.
- **An application that owns sw0** (a lock, one BLE connection) takes the
  fragment and the five-command board gate in `samples/door-lock-fragment/`.
- **That key is MCUboot's public dev key.** Fine on a desk, never in a
  product: set `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`.

## Three ways to send

| From     | Do this                                                                        | Over                |
|----------|--------------------------------------------------------------------------------|---------------------|
| Terminal | `tinyfota push update.wdfu`, add `--serial <port>` for a cable                 | Bluetooth or serial |
| Browser  | `python3 -m http.server -d web 8000`, open `localhost:8000` in Chrome or Edge  | Bluetooth or serial |
| Phone    | `tinyfota wrap`, open its `.zip` in nRF Device Manager                         | Bluetooth           |

## Numbers

Measured 2026-10-16 on the nRF52833 DK.

| What                              | Upstream Zephyr v4.4.2       | NCS v3.3.0                  |
|-----------------------------------|------------------------------|-----------------------------|
| Added to the bootloader           | 3,744 B flash, 1,088 B RAM   | 3,776 B flash, 1,072 B RAM  |
| Bootloader total                  | 32,840 B of 40,960 B         | 33,888 B of 40,960 B        |
| Staging area                      | 32 KB, of which 24 KB update | same                        |
| Update after changing one setting | 3,611 B, 1.91% of the image  | 3,645 B, 1.91% of the image |

The Bluetooth transports are separate Kconfig symbols: `TINYFOTA_BLE_SMP`
(SMP over GATT, what `tinyfota push`, the browser page and nRF Device Manager
speak), and the native `TINYFOTA_BLE_GATT` and `TINYFOTA_BLE_COC`, which no
shipped client uses and which are off by default. The sample builds SMP only;
`-DEXTRA_CONF_FILE=native.conf` adds the other two. Measured 2026-10-16 on the
DWM3001CDK, NCS v3.3.0, the sample's application image:

| Sample application                       | Flash     | RAM      |
|------------------------------------------|-----------|----------|
| SMP, native GATT and CoC (`native.conf`) | 190,508 B | 44,936 B |
| SMP only (the default)                   | 166,916 B | 41,624 B |
| Saved                                    | 23,592 B  | 3,312 B  |

The SMP-only build also drops `BT_SMP` and `BT_L2CAP_DYNAMIC_CHANNEL`, which
only the CoC needs.

On tinyble, a minimal BLE host that replaces Zephyr's, `TINYFOTA_TINYBLE`
serves SMP and the native frames itself, without mcumgr. The image group and
the reset gate are the same code the mcumgr build registers
(`ports/zephyr/smp_group.c`).

## Proof

| Test            | What it did                                                    | Result                                           |
|-----------------|----------------------------------------------------------------|--------------------------------------------------|
| `make powercut` | cut power at each of 3,263 flash steps, two ways: 6,526 runs   | every run ends on new or old image, never broken |
| `make fuzz`     | random input to headers, uploads and patching, 10 min a target | 2 crashes found and fixed, 0 open                |

## Limits

| Limit                                                              | Fixed in |
|--------------------------------------------------------------------|----------|
| Flash pages must be one size that divides 4 KB                     |          |
| Flash must write in 4 B words                                      | v0.2     |
| No rollback: a bad image needs a cable and MCUboot serial recovery | v0.3     |
| MCUboot on Zephyr only                                             | v0.4     |
| One update must fit in 24 KB                                       |          |
| Upstream Zephyr cannot write-lock MCUboot's flash                  |          |

## Develop

```sh
make test       # unit tests
make asan       # same, under sanitizers
make seams      # the port's symbols match tests/seams.expected
make powercut   # power-cut simulator, about 45 s
make wire       # browser and CLI send identical bytes (needs node)
make tinyble ZCBOR=<zcbor>   # the tinyble transport over real zcbor
```

If `cc` or `nm` is not a working tool: `make test CC=/usr/bin/clang NM="xcrun nm"`.

## Docs

| File             | What                                      |
|------------------|-------------------------------------------|
| `FORMAT.md`      | every byte on the wire and in flash       |
| `PORTING.md`     | bringing up a new chip                    |
| `THIRD_PARTY.md` | vendored code, licenses, local patches    |

## License

ISC (`LICENSE`). Third-party code: `THIRD_PARTY.md`.
