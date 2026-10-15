# tinyfota update format, version 1

This is the format ultrawidelock@d6e23761 shipped, unchanged byte for byte. A
patch built by that project's tool applies on a tinyfota board signed with the
same key, and the reverse.

Two things carry it: the `.wdfu` file that travels to the device, and the
staging partition the bootloader reads. `core/tinyfota.h` is the C definition;
`tools/tinyfota` writes it with `struct.pack("<IHHIIIII", ...)`.

## The `.wdfu` file

| Offset | Size | Contents |
|---|---|---|
| 0 | 32 B | header (below) |
| 32 | 64 B | ECDSA P-256 signature over the 32 header bytes: SHA-256, raw `r \|\| s`, big-endian |
| 96 | `patch_len` | the detools in-place patch |

The signing key is the MCUboot image-signing key. The application carries its
public half, generated at build time by `zephyr/scripts/emit_pubkey.py`.

## Header, 32 B, little-endian

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | u32 | `magic` | `0x55464457`, the bytes `57 44 46 55` ("WDFU") |
| 4 | u16 | `abi_version` | `1` |
| 6 | u16 | `flags` | `0` = delta. `0x0001` = whole image (`TINYFOTA_FLAG_FULL_IMAGE`), reserved for two-slot ports. The MCUboot applier does not install one. |
| 8 | u32 | `patch_len` | bytes of patch after the signature |
| 12 | u32 | `to_len` | size of the image the patch produces |
| 16 | u32 | `patch_crc32` | CRC-32 of the patch bytes |
| 20 | u32 | `from_crc32` | CRC-32 of the first `from_len` bytes of the primary slot |
| 24 | u32 | `from_len` | how much of the primary slot `from_crc32` covers |
| 28 | u32 | `hdr_crc32` | CRC-32 of bytes 0 to 27 |

CRC-32 is the IEEE 802.3 reflected polynomial `0xEDB88320` with init and final
XOR `0xFFFFFFFF`, which is Python's `zlib.crc32`.

The signature covers all 32 bytes, so no field can change without the key.
`hdr_crc32` is the last field and is written last, so a torn header write fails
its own check. The patch bytes are tied to the signature only through
`patch_len` and `patch_crc32`, and a CRC is not a cryptographic hash.

Example, the header the host suite's real-signature vector uses
(`tests/host/gen_dfu_vector.py`), 70 B patch, no base:

```
57 44 46 55  01 00  00 00  46 00 00 00  00 00 00 00
5f a5 b0 e9  00 00 00 00  00 00 00 00  90 39 eb 91
```

## The patch

detools in-place, created by `tinyfota build` with:

| Parameter | Value | Why |
|---|---|---|
| `patch_type` | `in-place` | the new image is written over the old one in the same slot |
| `compression` | `heatshrink`, window 8, lookahead 7 | the bootloader's decoder is static; `detools.c` rejects any other window or lookahead at the first byte |
| `memory_size` | the primary slot size (`mcuboot_primary`, header included) | |
| `segment_size` | 4096 B | the erase page |
| `minimum_shift_size` | 8192 B (two segments) | detools' floor; must stay a multiple of the segment size |

From and to are whole SIGNED MCUboot images, header and TLVs included, exactly
as flashed: the patch has to reproduce the signature too, or MCUboot would
refuse the result. Both must fit in `memory_size` with the shift slack to
spare. The bootloader compiles only the `none` and `heatshrink` decoders, so an
`lzma` patch fails at its first byte, before anything is erased.

## The staging partition

| Page | Offset | Contents | Written by |
|---|---|---|---|
| 0 | `0x0000` | the 32 B header. The signature is not stored. | receiver, LAST, after the patch CRC checks out |
| 1 | `0x1000` | the step log: one u32 per completed detools step, appended in order; `0xFFFFFFFF` marks the end | applier |
| 2 onward | `0x2000` | the patch, padded with `0xFF` to a 4 B boundary | receiver |

Patch capacity is the partition size less 8192 B. Pages are 4096 B and writes
are 4 B words; `PORTING.md` says what that asks of a chip.

No valid magic in page 0 means nothing is staged: that is the normal-boot fast
path, one word read. The applier erases the whole partition when the apply
completes, when the header fails its CRC, and when the primary slot is not the
image `from_crc32` names.

## The frame protocol

Used over an L2CAP CoC (PSM `0x0081`, 256 B SDUs) and over GATT (service
`d3b5a140-9e23-4b3a-8be4-6b1ee5f980a3`, characteristic
`d3b5a141-9e23-4b3a-8be4-6b1ee5f980a3`: write, write without response, notify).
All integers are little-endian u32.

| Request | Byte 0 | Then |
|---|---|---|
| BEGIN | `0x11` | transfer id, total wire length (the whole `.wdfu`) |
| DATA | `0x12` | transfer id, offset, bytes |
| COMMIT | `0x13` | transfer id. The device replies, then reboots after 500 ms. |
| ABORT | `0x14` | transfer id. Erases staging. |

| Reply | Byte 0 | Then |
|---|---|---|
| OK | `0x81` | transfer id, next offset the device expects |
| ERR | `0x82` | one error byte |

A DATA frame at any offset other than the device's own is answered with OK and
the device's real offset: a resynchronisation, not an error.

| Error | Code | Meaning |
|---|---|---|
| CLOSED | 1 | no update window is open |
| SEQUENCE | 2 | opcode does not fit the current state |
| SIZE | 3 | will not fit the staging partition |
| AUTH | 4 | header signature did not verify |
| INTEGRITY | 5 | length or CRC disagreed at commit |
| FLASH | 6 | a write or erase failed |
| MALFORMED | 7 | frame too short for its opcode |
| BUSY | 8 | another transport owns the receiver |
| BASE | 9 | signed for a different image than the one running. New in tinyfota; sent only after the signature has verified. |

## SMP

The Zephyr port also serves mcumgr's image group (1), so stock clients can
push an update: upload (command 1) takes the `.wdfu` or the wrapped `.bin`
below, the image list (command 0) reports one slot whose hash is the running
image's MCUboot SHA-256 TLV, and erase (command 5) clears staging. Group 0's
reset is refused unless the window is open or an update is staged.

## The MCUboot wrapper (`tinyfota wrap`)

Phone apps parse a file before they will upload it, and a raw `.wdfu` has no
magic they recognise. `wrap` puts a well-formed MCUboot image around it:

| Offset | Size | Contents |
|---|---|---|
| 0 | 32 B | MCUboot header: magic `0x96f3b83d`, `hdr_size` 32, `img_size` = `.wdfu` length, version from `--version`, build number = CRC-32 of the `.wdfu` |
| 32 | `.wdfu` length | the `.wdfu`, unchanged |
| then | 40 B | TLV info (`0x6907`, total 40) and a SHA-256 TLV over header and `.wdfu` |

The receiver recognises the magic, skips `hdr_size` bytes, takes `img_size`
bytes as the `.wdfu` and discards the trailer.

## Versions, and how v2 is recognised

`abi_version` sits at bytes 4 and 5, straight after the magic, and that is the
only promise v2 inherits: **a v2 file starts with the magic `WDFU` and then
`abi_version` = 2**. Everything after byte 5 may change, including the header
length, the staging layout and the patch parameters.

A v1 device never installs a v2 file:

1. If the v2 header is not exactly a v1-shaped 32 B header followed by a
   signature over it, the signature check fails (AUTH) before any erase.
2. If it is, the receiver refuses it at COMMIT (`abi_version != 1`,
   INTEGRITY), so no header is ever written to page 0. The worst it costs is
   one erase of the staging partition.
3. The applier also refuses any `abi_version` other than 1 and erases the
   staging partition without touching the primary slot.

Hosts read `abi_version` before sending (`tinyfota info` prints it) and should
refuse a file the device does not speak rather than lean on the above.

Bump `abi_version` for any change to the header, the staging layout, the CRC
or the patch parameters. A new `flags` bit needs no bump only if a v1 device
that ignores it stays safe.
