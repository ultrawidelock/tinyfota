<!-- DISCLAIMER: maintainer adds this -->

# tinyfota

Signed, power-cut-safe delta firmware updates for chips that cannot hold two
images. The patch is applied in place, from inside unmodified MCUboot, and
delivered from a phone app, a browser or a cable.

Extracted from ultrawidelock@d6e23761.

Status: scaffold. The sections below are filled in as the import lands.

## Build and test

```
make test     # host unit tests
make asan     # the same tests under AddressSanitizer and UndefinedBehaviorSanitizer
make seams    # the symbols the core expects a port to provide
```

On a machine where `cc` or `nm` is not the tool you want, pass both:
`make test CC=/usr/bin/clang NM="xcrun nm"`.

## Sizes

Measured on 2026-10-16 with NCS v3.3.0 (Zephyr 4.3.99), building
`samples/nrf52833` for both boards. Byte counts are `zephyr.bin` sizes and
`arm-zephyr-eabi-size` text and data, not estimates.

| What | nRF52833 DK | DWM3001CDK |
|---|---|---|
| Applier inside MCUboot | 3,680 B flash, 1,068 B RAM | not built separately |
| MCUboot total, against its partition | 33,792 B of 40,960 B (82.5%) | 33,548 B of 40,960 B (81.9%) |
| Staging partition | 32,768 B, of which 24,576 B is patch | same |
| Sample application, linked | 190,608 B of 433,664 B | 190,284 B of 433,664 B |
| Patch between two adjacent sample builds | 3,645 B (1.91% of the image) | not built |

- **Applier**: MCUboot built with and without `CONFIG_TINYFOTA_APPLIER` (30,112 B
  without). The linker map agrees: 3,668 B of code from the tinyfota library
  (detools 1,850 B, applier 1,008 B, heatshrink 574 B, flash port 192 B, CRC
  44 B) plus 12 B of alignment.
- **Adjacent builds**: image version 0.1.0, then 0.1.1 with one constant changed
  (`CONFIG_TINYFOTA_WINDOW_MS`, 300000 to 240000), which leaves 11 B of the
  unsigned image different. The patch carries that, the new header and the new
  signature TLVs.

## License

ISC, see `LICENSE`. Third-party code is listed in `THIRD_PARTY.md`.
