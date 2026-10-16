# tinyfota fragment for an application that owns sw0

The files an application copies to take tinyfota updates on an nRF52833
(DWM3001CDK) with NCS v3.3.0: MCUboot single slot with the applier, the
receiver on SMP over GATT only, no tinyfota button and one BLE connection. Not
an application: there is no `CMakeLists.txt` here. `samples/nrf52833` is the
reference these lines are cut from, and its files say why each line is there.

| File here | Goes to, in the application | What it is |
|---|---|---|
| `tinyfota.conf` | a Kconfig fragment in `CONF_FILE` (for example `stacks/tinyfota.conf`) | the receiver, SMP over Bluetooth, the reset gate |
| `sysbuild.conf` | `sysbuild.conf` | MCUboot, single slot, ECDSA P-256 |
| `sysbuild/mcuboot.conf` | `sysbuild/mcuboot.conf` | `BOOT_VALIDATE_SLOT0`, the applier, size trims |
| `pm_static.yml` | `pm_static.yml` | the four partitions; `make tooling` checks it against the sample's |

## Build

From the root of an NCS v3.3.0 workspace:

```
west build -p always -b decawave_dwm3001cdk --sysbuild -d <build> <app> -- \
  -DZEPHYR_EXTRA_MODULES="<other modules>;<tinyfota>" \
  -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=<absolute path to the signing key>
```

- `--sysbuild` builds MCUboot. `ZEPHYR_EXTRA_MODULES` on the command line
  reaches the MCUboot image too, which is how the applier gets in; nothing
  else needs passing.
- The signing key goes on the command line only, never in a file. Without
  it the build signs with MCUboot's published development key and warns.
  The receiver checks patches against the same key.
- Without copying the files, the same build takes them from here:
  `-DSB_CONF_FILE=<this dir>/sysbuild.conf`,
  `-Dmcuboot_EXTRA_CONF_FILE=<this dir>/sysbuild/mcuboot.conf`,
  `-DPM_STATIC_YML_FILE=<this dir>/pm_static.yml`, and `tinyfota.conf` added
  to `CONF_FILE`.

## What the application does

1. **Opens the update window** with `tinyfota_window_open(CONFIG_TINYFOTA_WINDOW_MS)`
   from its own owner gesture. Nothing is accepted before that. sw0 stays the
   application's (`CONFIG_TINYFOTA_BUTTON=n`).
2. **Advertises the SMP service** (`8d53dc1d-1db7-4cd3-868b-8a527460aa84`),
   connectable, while the window is open. `tinyfota push` and the browser page
   find a board by that UUID only, never by name. A 128-bit UUID is 18 B of
   the 31 B advertisement, so it can go in the scan response.
3. **Keeps `CONFIG_BT_MAX_CONN=1`** when updates and its own BLE use never
   overlap (`zephyr/Kconfig`, `TINYFOTA_BLE_SMP`).

tinyfota writes no settings keys. Its only state is the `patch_staging`
partition (`FORMAT.md`); `settings_storage` is untouched.

## MCUboot serial recovery (optional)

Not in the fragment. To reach MCUboot recovery over SMP, without a button, add
to the application `CONFIG_RETAINED_MEM=y`, `CONFIG_RETENTION=y`,
`CONFIG_RETENTION_BOOT_MODE=y` and `CONFIG_MCUMGR_GRP_OS_RESET_BOOT_MODE=y`;
to `sysbuild/mcuboot.conf` the `MCUBOOT_SERIAL`, `BOOT_SERIAL_UART`,
`UART_CONSOLE=n`, `BOOT_SERIAL_NO_APPLICATION`, retention and
`BOOT_SERIAL_BOOT_MODE` lines of `samples/nrf52833/sysbuild/mcuboot.conf`;
and to both images the GPREGRET2 retention node of
`samples/nrf52833/boards/decawave_dwm3001cdk.overlay`. tinyfota's reset gate
refuses the reset unless an update window is open or a patch is staged.

## Measured

2026-10-17, NCS v3.3.0, `decawave_dwm3001cdk`, LTO on, tinylock 67dfce9 with
its stock stacks, tinytalk 9604083:

| Image | Flash | RAM |
|---|---|---|
| Application, no MCUboot, no tinyfota | 298,956 B | 97,248 B |
| Application with `tinyfota.conf` | 308,064 B of 433,664 B | 101,216 B |
| Added by tinyfota and mcumgr | 9,108 B | 3,968 B |
| MCUboot with `sysbuild/mcuboot.conf` | 25,512 B of 40,960 B | 14,464 B |
