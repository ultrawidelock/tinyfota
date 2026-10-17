# tinyfota fragment for an application that owns sw0

The files an application copies to take tinyfota updates on an nRF52833
(DWM3001CDK) with NCS v3.3.0: MCUboot single slot with the applier, the
receiver on SMP over GATT only, no tinyfota button and one BLE connection. Not
an application: there is no `CMakeLists.txt` here. `samples/nrf52833` is the
reference these lines are cut from, and its files say why each line is there.

| File here | Goes to, in the application | What it is |
|---|---|---|
| `tinyfota.conf` | `stacks/tinyfota.conf`, last in `CONF_FILE` | the receiver, SMP over Bluetooth, the reset gate |
| `tinyfota-tinyble.conf` | `stacks/tinyfota-tinyble.conf`, last in `CONF_FILE`, INSTEAD of `tinyfota.conf` on a tinyble build | the receiver on tinyble: SMP and the native frames without mcumgr, the SMP UUID in the scan response |
| `sysbuild.conf` | `stacks/tinyfota/sysbuild.conf`, as `SB_CONF_FILE` | MCUboot, single slot, ECDSA P-256 |
| `mcuboot.conf` | `stacks/tinyfota/mcuboot.conf`, as `mcuboot_CONF_FILE` | MCUboot's whole configuration: `BOOT_VALIDATE_SLOT0`, the applier, size trims |
| `pm_static.yml` | `stacks/tinyfota/pm_static.yml`, as `PM_STATIC_YML_FILE` | the four partitions; `make tooling` checks it against the sample's |

`mcuboot.conf` REPLACES MCUboot's own `prj.conf` and board fragment rather
than extending them; its header says why. Pass it as `mcuboot_CONF_FILE`
with an absolute path, and do not put it at `<app>/sysbuild/mcuboot.conf`,
where sysbuild would also merge it as an extra fragment.

## Build

From the root of an NCS v3.3.0 workspace:

```
west build -p always -b decawave_dwm3001cdk --sysbuild -d <build> <app> -- \
  -DZEPHYR_EXTRA_MODULES="<other modules>;<tinyfota>" \
  -DCONF_FILE="prj.conf;<the application's stacks>;stacks/tinyfota.conf" \
  -DSB_CONF_FILE=stacks/tinyfota/sysbuild.conf \
  -DPM_STATIC_YML_FILE=stacks/tinyfota/pm_static.yml \
  -Dmcuboot_CONF_FILE=<absolute path to app>/stacks/tinyfota/mcuboot.conf \
  '-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE="<absolute path to the signing key>"'
```

- `--sysbuild` builds MCUboot. `ZEPHYR_EXTRA_MODULES` on the command line
  reaches the MCUboot image too, which is how the applier gets in.
- The signing key goes on the command line only, never in a file, and its
  value keeps the inner double quotes: sysbuild reads it as a Kconfig string
  and stops with "malformed string literal" without them. Without the flag
  the build signs with MCUboot's published development key and warns. The
  receiver checks patches against the same key.
- The build prints no "was assigned" Kconfig warning. What it still prints:
  four `Deprecated symbol PARTITION_MANAGER(_ENABLED) is enabled` lines, which
  NCS v3.3.0 prints for every build that uses Partition Manager, and therefore
  `pm_static.yml`; CMake warnings for the development key and NCS's
  secure-boot and Partition Manager notices.
- Two warnings are fixed outside these files. `sysbuild.conf` turns off
  `SB_CONFIG_MCUBOOT_APP_SYNC_UPDATEABLE_IMAGES`, so sysbuild stops assigning
  `CONFIG_UPDATEABLE_IMAGE_NUMBER` to an application that has no image
  manager. `zephyr/Kconfig` gives `MCUBOOT_UPDATE_FOOTER_SIZE` a prompt in a
  single-slot receiver build, so the value MCUboot's sysbuild module assigns
  to every application lands; its comment says why there is no other way.

### On tinyble

With tinyble in place of Zephyr's Bluetooth host, `tinyfota-tinyble.conf`
replaces `tinyfota.conf` and the other three files stay as they are. tinyble
must carry `CONFIG_TINYBLE_TINYFOTA` (the fragment sets it), which adds the
SMP and native services to its GATT table and hands their writes to
`CONFIG_TINYFOTA_TINYBLE`. No mcumgr is built; `zephyr/Kconfig` says what the
transport serves.

```
west build -p always -b decawave_dwm3001cdk --sysbuild -d <build> <app> -- \
  -DZEPHYR_EXTRA_MODULES="<tinytalk>;<tinyble>;<tinygrid>;<tinypsa>;<tinyfota>" \
  -DCONF_FILE="prj.conf;stacks/tinyble.conf;stacks/tinygrid.conf;stacks/tinypsa.conf;stacks/tinyfota-tinyble.conf" \
  -DSB_CONF_FILE=stacks/tinyfota/sysbuild.conf \
  -DPM_STATIC_YML_FILE=stacks/tinyfota/pm_static.yml \
  -Dmcuboot_CONF_FILE=<absolute path to app>/stacks/tinyfota/mcuboot.conf \
  '-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE="<absolute path to the signing key>"'
```

`tinyfota push update.wdfu` speaks SMP to it as to the Zephyr-host build;
`tinyfota push --native update.wdfu` sends the native frames instead.

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

## Board gate: five commands

From the root of an NCS v3.3.0 workspace, with the four files at the paths in
the table above, and `tinyfota` installed (`pip install ./tools/tinyfota`).
`<key>` is the absolute path to the signing key, the same in every command,
and `<app>` is absolute.

```
# 1. the base image, built and flashed; keep this build directory
west build -p always -b decawave_dwm3001cdk --sysbuild -d <build>/fota-v1 <app> -- \
  -DZEPHYR_EXTRA_MODULES="<tinytalk>;<tinyfota>" \
  -DCONF_FILE="prj.conf;stacks/bt-host.conf;stacks/openthread.conf;stacks/nrf-security.conf;stacks/tinyfota.conf" \
  -DSB_CONF_FILE=stacks/tinyfota/sysbuild.conf -DPM_STATIC_YML_FILE=stacks/tinyfota/pm_static.yml \
  -Dmcuboot_CONF_FILE=<app>/stacks/tinyfota/mcuboot.conf \
  '-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE="<key>"' && west flash -d <build>/fota-v1

# 2. the changed image: the same line with one setting changed, the image version
west build -p always -b decawave_dwm3001cdk --sysbuild -d <build>/fota-v2 <app> -- \
  -DZEPHYR_EXTRA_MODULES="<tinytalk>;<tinyfota>" \
  -DCONF_FILE="prj.conf;stacks/bt-host.conf;stacks/openthread.conf;stacks/nrf-security.conf;stacks/tinyfota.conf" \
  -DSB_CONF_FILE=stacks/tinyfota/sysbuild.conf -DPM_STATIC_YML_FILE=stacks/tinyfota/pm_static.yml \
  -Dmcuboot_CONF_FILE=<app>/stacks/tinyfota/mcuboot.conf \
  '-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE="<key>"' '-DCONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="1.0.1+0"'

# 3. the delta; `info` prints the line to record (abi, patch bytes, file bytes)
tinyfota build --from <build>/fota-v1/door-lock/zephyr/zephyr.signed.hex \
  --to <build>/fota-v2/door-lock/zephyr/zephyr.signed.hex \
  --build-dir <build>/fota-v2 --key <key> --out update.wdfu && tinyfota info update.wdfu

# 4. open the update window on the lock, then push over BLE
tinyfota push update.wdfu

# 5. after MCUboot applies it: the board runs v2's SHA-256 (and --list shows 1.0.1)
tinyfota push --expect <build>/fota-v2/door-lock/zephyr/zephyr.signed.bin
```

The application image's directory is named after `<app>`: `door-lock` here.
`tinyfota push` finds the board by the SMP service UUID, not by name, and
prints the name it connects to (`tinytalk`). With more than one board
advertising SMP in range it lists them; pick one with `--address`. Steps 4
and 5 need the application side above: the window gesture and the SMP
advertisement.

Steps 1 to 3 measured host-side 2026-10-17, on a snapshot of tinylock 7d468f5
with tinytalk 9604083 and MCUboot's development key: both images 307,652 B
flash / 101,216 B RAM, `record  abi 1, patch 5,775 B, file 5,871 B`, 23.5% of
the 24,576 B patch capacity. Steps 4 and 5 are not yet run on a board.

## MCUboot serial recovery (optional)

Not in the fragment. To reach MCUboot recovery over SMP, without a button, add
to the application `CONFIG_RETAINED_MEM=y`, `CONFIG_RETENTION=y`,
`CONFIG_RETENTION_BOOT_MODE=y` and `CONFIG_MCUMGR_GRP_OS_RESET_BOOT_MODE=y`;
to `mcuboot.conf` the `MCUBOOT_SERIAL`, `BOOT_SERIAL_UART`,
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
| MCUboot with `mcuboot.conf` | 25,512 B of 40,960 B | 14,464 B |

Re-measured 2026-10-17, NCS v3.3.0, `decawave_dwm3001cdk`, on tinylock b33f550
with tinytalk 4e14cfc and MCUboot's development key, with the command above
and this directory at tinyfota's commit that added `tinyfota-tinyble.conf`:

| Build | Application | MCUboot |
|---|---|---|
| Zephyr host, `tinyfota.conf` | 314,176 B flash / 101,408 B RAM | 25,512 B / 14,464 B |
| tinyble 8ecda5d, tinygrid b8e3b4a, tinypsa d556143, `tinyfota-tinyble.conf` | 215,824 B flash / 70,680 B RAM | 25,512 B / 14,464 B |

In the tinyble build the transport's buffers are 588 B of RAM (`s_smp`: the
384 B request, the 192 B reply and their counters) and its two work items
48 B each. Both builds print no "was assigned" Kconfig warning.

Re-measured 2026-10-17 on a snapshot of tinylock 46a7115 with the layout and
command above: application 307,652 B / 101,216 B, MCUboot 25,512 B /
14,464 B, 0 "was assigned" warnings. MCUboot's generated `.config` is
identical to the one built by merging this file on top of MCUboot's
`prj.conf`.
