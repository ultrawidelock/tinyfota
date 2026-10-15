# Porting tinyfota

A port is two things: the functions in `include/tinyfota/tinyfota_port.h`, and
a chip and bootloader that meet the requirements below. `ports/zephyr/` is the
target port and `tests/host/port/` holds the host fakes; read both side by
side.

## The seams

`make seams` compiles the core, takes the globals it uses but does not define,
and compares them with `tests/seams.expected`. These 17 symbols are the whole
list:

| Seam | Used by | Contract |
|---|---|---|
| `tinyfota_flash_open`, `tinyfota_flash_close` | applier, receiver | open `TINYFOTA_FLASH_AREA_PRIMARY` (the running image's slot) or `TINYFOTA_FLASH_AREA_STAGING` (where a patch lands). Return negative on failure. The area type is opaque, so the port owns its layout |
| `tinyfota_flash_size` | applier, receiver | the area's size in bytes |
| `tinyfota_flash_read` | applier, receiver | any offset and length. Return negative when the range is outside the area |
| `tinyfota_flash_write` | applier, receiver | offset and length are 4 B aligned |
| `tinyfota_flash_erase` | applier, receiver | to 0xff; offset and length are page aligned |
| `tinyfota_reboot` | receiver | cold reboot into the bootloader, which applies the staged patch |
| `tinyfota_uptime_ms` | receiver | monotonic milliseconds, for the update window |
| `tinyfota_lock`, `tinyfota_unlock` | receiver | one lock around the receiver, because each transport calls it from its own context. A single-threaded port may make both no-ops |
| `tinyfota_timer_init`, `_schedule`, `_reschedule`, `_cancel` | receiver | two deferred jobs, `TINYFOTA_TIMER_REBOOT` (reboot after the COMMIT reply has gone out) and `TINYFOTA_TIMER_WINDOW` (close the window). `_schedule` must not run the job from inside the call |
| `tinyfota_ecdsa_p256_verify` | receiver | ECDSA P-256 over SHA-256 of the message; public key 0x04 \|\| X \|\| Y, signature raw r \|\| s. Return 0 only when it verifies. Any crypto provider init happens in here, once |
| `tinyfota_pubkey` | receiver | 65 B constant, the public half of the image-signing key. `zephyr/scripts/emit_pubkey.py` generates it at build time |
| `tinyfota_log` | either, optional | only with `TINYFOTA_PORT_LOG=1`. Level 1 (error) to 4 (debug) |

Flash geometry is not a seam. The erase page and the write block are
compile-time constants of the format (4,096 B and 4 B), and the Zephyr port
checks the device against them when it opens an area.

The applier needs only the flash seams, because it runs inside the
bootloader. The receiver, in the application, needs all of them.

## What a new chip must satisfy

1. **Erase pages that divide 4,096 B**, the same size across both partitions.
   The patch is cut into 4,096 B segments and each one is erased as a unit.
2. **A write block that divides 4 B**, where a word can be written once after
   an erase. Flash that writes in 8 B or larger units, or error-correcting
   flash that cannot rewrite a word, waits for v0.2.
3. **A bootloader hook that runs before the bootloader validates the primary
   slot and before it decides on recovery**, with the flash driver already up.
   On Zephyr that is `SYS_INIT` at `APPLICATION` level, ahead of MCUboot's
   `main()`. Elsewhere, `TINYFOTA_INIT_APPLICATION_PRIO` exports
   `tinyfota_init_<fn>` for the bootloader to call.
4. **A bootloader that checks the result.** MCUboot needs single-slot mode,
   `CONFIG_BOOT_VALIDATE_SLOT0` and serial recovery with
   `CONFIG_BOOT_SERIAL_NO_APPLICATION`. Without the validation, a forged patch
   could install code.
5. **Room in the bootloader.** On the nRF52833 the applier costs 3,776 B of
   flash and 1,072 B of RAM (`README.md`, Sizes). Its state is static rather
   than on the stack, because MCUboot still has an ECDSA verify to run after
   it returns.
6. **A staging partition** big enough for the largest delta you will ship,
   plus 8,192 B: one page for the header and one for the step log
   (`FORMAT.md`). The sample uses 32,768 B, of which 24,576 B is patch.
7. **In the application:** a P-256 verify, a transport (the Zephyr port has
   L2CAP, GATT and SMP), and an owner gesture that opens the update window.

## The sample layout

`samples/nrf52833/pm_static.yml` on NCS and `samples/nrf52833/partitions.dtsi`
on upstream Zephyr, the same addresses in both (`make tooling` checks), for
512 KB of flash:

| Partition | Address | Size |
|---|---|---|
| `mcuboot` | 0x0 | 40,960 B (0xa000) |
| `mcuboot_primary` | 0xa000 | 434,176 B (0x6a000), the image header included |
| `patch_staging` | 0x74000 | 32,768 B (0x8000) |

`tinyfota build --build-dir` reads the slot and staging sizes from the build's
`partitions.yml` on NCS, or its `zephyr.dts` on upstream Zephyr. Without a
build directory, pass `--memory-size` and `--staging-size`.

## Checking a port

- `make seams` must list exactly the 17 symbols above.
- The host suite (`make test`, `make asan`) runs the core over the host fakes
  with the nRF alignment rules. A new port's flash should follow the same
  rules the fakes enforce in `tests/host/port/flash_host.c`.
- `make powercut` exercises the 4,096 B page and 4 B word model. A chip
  outside that model needs v0.2 first.
