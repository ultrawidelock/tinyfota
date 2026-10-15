# tinyfota sample: nRF52833

The smallest application that takes a delta update. It builds for two boards:

| Board | `-b` |
|---|---|
| nRF52833 DK | `nrf52833dk/nrf52833` |
| Qorvo (Decawave) DWM3001CDK | `decawave_dwm3001cdk` |

What it does:

- advertises the SMP service, and the native update service in its scan
  response, so every updater finds it by service UUID;
- keeps a second connection slot free for an updater, and refuses to build
  with `CONFIG_BT_MAX_CONN` below 2;
- receives a patch over its own L2CAP CoC, a GATT characteristic, SMP over
  Bluetooth, or SMP over uart0;
- accepts nothing until **sw0** opens a 300 s update window, and lights
  **led0** while it is open;
- says at every boot which image it runs (version and MCUboot SHA-256, on RTT
  and in the SMP image list), which is what a host checks to confirm an update.
  Single-slot MCUboot has no test state to confirm out of: MCUboot validated the
  image before jumping to it;
- holding sw0 for 5 s reboots into MCUboot serial recovery.

MCUboot runs single-slot with serial recovery on uart0, `BOOT_VALIDATE_SLOT0`,
and tinyfota's applier. `sysbuild/mcuboot.conf` says why each line is there.

## Build

From the root of an NCS v3.3.0 workspace (`nrfutil sdk-manager install v3.3.0`
makes one):

```
west build -b nrf52833dk/nrf52833 --sysbuild -d build/tinyfota <tinyfota>/samples/nrf52833
west flash -d build/tinyfota
```

`sysbuild/CMakeLists.txt` adds the tinyfota module to both images, so nothing
else needs passing. When tinyfota is already a project in your west manifest,
add `-- -DTINYFOTA_IN_MANIFEST=y`.

The sample signs with MCUboot's published development key unless you set
`SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`, and the build warns about it. Patches are
signed with the same key: the receiver defaults `TINYFOTA_KEY` to the key the
image is signed with.

## Update it

Keep `build/tinyfota/nrf52833/zephyr/zephyr.signed.hex`: it is the base every
later patch for this board is built from, and it cannot be rebuilt
byte-for-byte. Then change something, rebuild into another directory, and:

```
tinyfota build --from old/zephyr.signed.hex --to new/zephyr.signed.hex \
    --build-dir build/tinyfota --key <the signing key> --out update.wdfu
tinyfota push update.wdfu            # press sw0 when it asks
```

Logs are on RTT; uart0 carries SMP.
