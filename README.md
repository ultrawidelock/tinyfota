# tinyfota

Firmware updates over Bluetooth for chips too small to hold two copies of
their firmware. Signed. Survives power cuts. Sends only what changed.

```
nRF52833, 512 KB

+---------+-------------------------------------+---------+--------+
| MCUboot | app, rewritten in place             | staging | config |
|  40 KB  | 424 KB                              |  32 KB  |  16 KB |
+---------+-------------------------------------+---------+--------+
```

The app stages a signed delta. On reboot a hook in unmodified MCUboot
rewrites the app in place, journaled, so a power cut resumes instead of
bricking.

4.7 KB flash, 1.0 KB RAM in the app and 3.7 KB flash, 1.0 KB RAM in
MCUboot, LTO release build. A one-setting change ships as 3.5 KB.

## Quick start

```sh
python3 -m venv .venv && . .venv/bin/activate && pip install ./tools/tinyfota

west build -b nrf52833dk/nrf52833 --sysbuild -d build/v1 <tinyfota>/samples/nrf52833
west flash -d build/v1
# change something
west build -b nrf52833dk/nrf52833 --sysbuild -d build/v2 <tinyfota>/samples/nrf52833

tinyfota build --from build/v1/nrf52833/zephyr/zephyr.signed.hex \
    --to build/v2/nrf52833/zephyr/zephyr.signed.hex \
    --build-dir build/v2 --key <workspace>/bootloader/mcuboot/root-ec-p256.pem \
    --out update.wdfu
tinyfota push update.wdfu   # press sw0 when asked
```

Keep the `.hex` of every build you ship: later updates start from it. The
key above is MCUboot's public dev key; ship your own with
`SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`.

Also sends over a cable (`--serial <port>`), from Chrome (`web/`), or from
nRF Device Manager (`tinyfota wrap`).

## Test

```sh
make test       # also asan, seams, wire
make powercut   # a power cut at each of 3,263 flash steps, never bricked
make fuzz
```

## Limits

- No rollback yet: a bad image needs a cable.
- One update fits in 24 KB. MCUboot on Zephyr only.
- Upstream Zephyr cannot write-lock MCUboot's flash.

Apache-2.0. Experimental, no warranty.

Unofficial. Not affiliated with, endorsed or certified by any company or
standards body named here. Trademarks belong to their owners.
