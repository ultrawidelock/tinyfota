/* SPDX-License-Identifier: ISC */

/**
 * @file
 * @brief The on-flash contract between the application and the bootloader for
 *        a delta firmware update.
 *
 * The application receives a patch over Bluetooth and writes it into the
 * `patch_staging` partition. MCUboot reads it on the next boot and applies it
 * onto the primary slot. Nothing else connects the two, so this header IS the
 * interface: a change here that is not made on both sides produces a board
 * that stages an update and then silently declines to install it.
 *
 * Plain C11 with no Zephyr dependency, so the host tests and the patch builder
 * can include it and agree on the layout by construction rather than by
 * transcription.
 */

#ifndef TINYFOTA_H_
#define TINYFOTA_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** "WDFU" read as a little-endian word. */
#define TINYFOTA_MAGIC 0x55464457u

/** Bumped whenever the layout below changes in any way. */
#define TINYFOTA_ABI_VERSION 1u

/**
 * The payload is a whole application image, not a detools delta.
 *
 * The ESP32 has two full OTA slots and no MCUboot, so it has no use for a
 * delta and no applier to run one; what it wants is the image itself, written
 * into the slot it is not currently booted from. Everything else about the
 * container is unchanged -- same magic, same ABI, same 64-byte P-256 signature
 * over the same 32 header bytes, same CRC-32 over the payload -- so one signer,
 * one receiver and one wire protocol serve both chips.
 *
 * NOT AN ABI BUMP, because nothing about the layout changed: this occupies the
 * `flags` word that was already reserved and already covered by both the header
 * CRC and the signature, so it cannot be flipped by anyone without the key.
 *
 * A port MUST reject a payload whose flag does not match what it can apply. A
 * delta handed to the ESP32 would be written into an OTA slot as if it were an
 * image; a whole image handed to the CDK does not fit patch_staging at all and
 * is refused for size long before this matters, but the check is cheap and the
 * failure should say what is actually wrong.
 */
#define TINYFOTA_FLAG_FULL_IMAGE 0x0001u

/**
 * Layout of the staging partition. Page-granular because the erase unit is a
 * page, and the three regions have different write patterns: the header is
 * written once, the step log is appended to during the apply, and the patch is
 * written once. Sharing a page between any two of them would mean erasing one
 * to update another.
 */
#define TINYFOTA_PAGE_SIZE 4096u

/** Page 0: @ref tinyfota_hdr, written by the application. */
#define TINYFOTA_HDR_OFFSET 0u

/**
 * Page 1: the step log, written by the bootloader.
 *
 * One 32-bit word per completed patch step, appended in order, never erased
 * mid-apply. An erased word (0xffffffff) marks the end. This is what makes a
 * power cut survivable: detools re-reads the last completed step and skips
 * everything already done (`detools.c:1559`) instead of restarting the patch
 * against a half-patched image.
 */
#define TINYFOTA_STEP_OFFSET TINYFOTA_PAGE_SIZE

/** Page 2 onward: the patch itself. */
#define TINYFOTA_PATCH_OFFSET (2u * TINYFOTA_PAGE_SIZE)

/** Value of an erased flash word, and so the end marker of the step log. */
#define TINYFOTA_STEP_ERASED 0xffffffffu

/**
 * The staged-update header.
 *
 * Integrity here is CRC-32, not a hash, and that is deliberate. AUTHENTICITY is
 * checked by the APPLICATION, which can afford PSA ECDSA-P256, before it
 * ever writes this header. The bootloader is the flash-starved
 * image and only has to answer a narrower question: did the bytes I am about to
 * apply arrive intact, and do they belong to the image I am running? A CRC
 * answers both.
 *
 * The floor underneath both is MCUboot's own image validation:
 * `CONFIG_BOOT_VALIDATE_SLOT0=y` re-verifies the P-256 signature of the
 * RESULT before booting it. So a forged header cannot install code -- it can
 * only destroy the current image, and `CONFIG_BOOT_SERIAL_NO_APPLICATION=y`
 * catches that in recovery rather than in a boot loop.
 *
 * Every field is little-endian. Total 32 bytes, word-aligned throughout,
 * because the nRF flash driver writes words.
 */
struct tinyfota_hdr {
	/** @ref TINYFOTA_MAGIC. Anything else means "no update staged", which is
	 *  the normal-boot fast path and must stay a single word read. */
	uint32_t magic;
	/** @ref TINYFOTA_ABI_VERSION. */
	uint16_t abi_version;
	/** @ref TINYFOTA_FLAG_FULL_IMAGE, or zero for a delta. */
	uint16_t flags;
	/** Patch length in bytes, starting at @ref TINYFOTA_PATCH_OFFSET. */
	uint32_t patch_len;
	/** Size the primary slot's image is expected to have afterwards. */
	uint32_t to_len;
	/** CRC-32 over the @ref patch_len patch bytes. */
	uint32_t patch_crc32;
	/** CRC-32 over the first @ref from_len bytes of the primary slot. */
	uint32_t from_crc32;
	/** How much of the primary slot @ref from_crc32 covers. */
	uint32_t from_len;
	/** CRC-32 over the 28 bytes above. Written last, so a torn write leaves
	 *  a header that fails its own check rather than one that looks valid. */
	uint32_t hdr_crc32;
};

/**
 * Bytes of @ref tinyfota_hdr covered by @ref tinyfota_hdr.hdr_crc32: everything
 * ahead of the field itself. The whole struct is 32 bytes, so this is 28.
 */
#define TINYFOTA_HDR_CRC_LEN 28u

/** Wire and on-flash size of @ref tinyfota_hdr. Asserted against sizeof(). */
#define TINYFOTA_HDR_LEN 32u

/** Raw P-256 signature, r||s, over all @ref TINYFOTA_HDR_LEN header bytes. */
#define TINYFOTA_SIG_LEN 64u

#ifdef __cplusplus
}
#endif

#endif /* TINYFOTA_H_ */
