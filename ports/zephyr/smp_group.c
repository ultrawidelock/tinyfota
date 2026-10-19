/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief The SMP image group's commands and the reset gate, transport-neutral.
 *
 * Split out of ports/zephyr/dfu_smp_img.c so that two SMP servers share one
 * copy: mcumgr on the Zephyr Bluetooth host (dfu_smp_img.c) and tinyfota's own
 * server on tinyble (dfu_tinyble.c). Nothing here knows which one called. The
 * caller has opened the response map and closes it; on a non-zero return it
 * throws the partial response away and sends {"rc": status} instead, which is
 * what mcumgr does.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zcbor_common.h>
#include <zcbor_encode.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "smp_group.h"
#include "tinyfota.h"
#include "tinyfota_rx.h"
#include "tinyfota/tinyfota_partitions.h"
#include "tinyfota/tinyfota_port.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(tinyfota, CONFIG_TINYFOTA_LOG_LEVEL);

/* ---- the running image's identity ----------------------------------------- */

#define IMAGE_MAGIC               0x96f3b83d
#define IMAGE_TLV_INFO_MAGIC      0x6907
#define IMAGE_TLV_PROT_INFO_MAGIC 0x6908
#define IMAGE_TLV_SHA256          0x10
#define IMAGE_SHA_LEN             32

/**
 * MCUboot image version: major, minor, revision, and build number.
 */
struct image_version {
	uint8_t iv_major;
	uint8_t iv_minor;
	uint16_t iv_revision;
	uint32_t iv_build_num;
};

/**
 * MCUboot image header: magic, load address, header size, protected TLV size, image size, flags,
 * version (major, minor, revision, build), and padding.
 */
struct image_header {
	uint32_t ih_magic;
	uint32_t ih_load_addr;
	uint16_t ih_hdr_size;
	uint16_t ih_protect_tlv_size;
	uint32_t ih_img_size;
	uint32_t ih_flags;
	struct image_version ih_ver;
	uint32_t _pad1;
};

/**
 * MCUboot image TLV info: magic marker (0x6907) and total TLV region size in bytes.
 */
struct image_tlv_info {
	uint16_t it_magic;
	uint16_t it_tlv_tot;
};

/**
 * MCUboot image TLV entry: type, padding byte, and length in bytes.
 */
struct image_tlv {
	uint8_t it_type;
	uint8_t _pad;
	uint16_t it_len;
};

/* Read straight through a pointer rather than flash_area_read(): this is the
 * image that is currently executing, so it is already mapped at a known address
 * and a copy would only cost stack. */
static const struct image_header *running_header(void)
{
	const struct image_header *h =
		(const struct image_header *)TINYFOTA_PARTITION_ADDRESS(slot0_partition);

	return (h->ih_magic == IMAGE_MAGIC) ? h : NULL;
}

/**
 * Find the SHA-256 that MCUboot recorded for the running image.
 *
 * This is the same hash the bootloader verifies against, so a client that
 * records it before an update can tell afterwards whether the update landed.
 * Returns NULL if the TLV block is not where the header says it is, which is
 * reported to the client as an all-zero hash rather than a refusal.
 */
static const uint8_t *running_hash(const struct image_header *h)
{
	const uint8_t *base = (const uint8_t *)h + h->ih_hdr_size + h->ih_img_size;
	const struct image_tlv_info *info = (const struct image_tlv_info *)base;
	const uint8_t *p;
	const uint8_t *end;

	/* The protected block, when present, comes first and carries its own
	 * info header; the unprotected one follows it. */
	if (info->it_magic == IMAGE_TLV_PROT_INFO_MAGIC) {
		base += info->it_tlv_tot;
		info = (const struct image_tlv_info *)base;
	}
	if (info->it_magic != IMAGE_TLV_INFO_MAGIC) {
		return NULL;
	}

	p = base + sizeof(*info);
	end = base + info->it_tlv_tot;

	while (p + sizeof(struct image_tlv) <= end) {
		const struct image_tlv *tlv = (const struct image_tlv *)p;

		p += sizeof(*tlv);
		if (p + tlv->it_len > end) {
			return NULL;
		}
		if (tlv->it_type == IMAGE_TLV_SHA256 && tlv->it_len == IMAGE_SHA_LEN) {
			return p;
		}
		p += tlv->it_len;
	}
	return NULL;
}

/* ---- image list ----------------------------------------------------------- */

/**
 * Encode the one slot this board has.
 *
 * Deliberately shaped exactly like Zephyr's img_mgmt_state_encode_slot(), keys
 * and order included, so a client cannot distinguish the two.
 */
static bool encode_slot(zcbor_state_t *zse)
{
	const struct image_header *h = running_header();
	uint8_t zeros[IMAGE_SHA_LEN] = {0};
	const uint8_t *hash = NULL;
	char vers[24];
	struct zcbor_string zhash;
	bool ok;

	if (h != NULL) {
		hash = running_hash(h);
		(void)snprintf(vers, sizeof(vers), "%u.%u.%u.%u", h->ih_ver.iv_major,
			       h->ih_ver.iv_minor, h->ih_ver.iv_revision,
			       (unsigned)h->ih_ver.iv_build_num);
	} else {
		(void)snprintf(vers, sizeof(vers), "0.0.0.0");
	}

	zhash.value = (hash != NULL) ? hash : zeros;
	zhash.len = IMAGE_SHA_LEN;

	ok = zcbor_map_start_encode(zse, 8) && zcbor_tstr_put_lit(zse, "image") &&
	     zcbor_uint32_put(zse, 0) && zcbor_tstr_put_lit(zse, "slot") &&
	     zcbor_uint32_put(zse, 0) && zcbor_tstr_put_lit(zse, "version") &&
	     zcbor_tstr_put_term(zse, vers, sizeof(vers)) && zcbor_tstr_put_lit(zse, "hash") &&
	     zcbor_bstr_encode(zse, &zhash) && zcbor_tstr_put_lit(zse, "bootable") &&
	     zcbor_bool_put(zse, true) && zcbor_tstr_put_lit(zse, "pending") &&
	     zcbor_bool_put(zse, false) && zcbor_tstr_put_lit(zse, "confirmed") &&
	     zcbor_bool_put(zse, true) && zcbor_tstr_put_lit(zse, "active") &&
	     zcbor_bool_put(zse, true) && zcbor_tstr_put_lit(zse, "permanent") &&
	     zcbor_bool_put(zse, true) && zcbor_map_end_encode(zse, 8);

	return ok;
}

/**
 * Encode and send the image state reply containing one slot (image 0) with splitStatus 0. Returns
 * MGMT_ERR_EOK on success or MGMT_ERR_EMSGSIZE if the response overflows.
 */
int tinyfota_smp_img_state_read(zcbor_state_t *zse)
{
	bool ok;

	ok = zcbor_tstr_put_lit(zse, "images") && zcbor_list_start_encode(zse, 1) &&
	     encode_slot(zse) && zcbor_list_end_encode(zse, 1) &&
	     zcbor_tstr_put_lit(zse, "splitStatus") && zcbor_int32_put(zse, 0);

	return ok ? TINYFOTA_SMP_RC_OK : TINYFOTA_SMP_RC_EMSGSIZE;
}

/**
 * Accept "set state" and change nothing.
 *
 * A client sends this to mark an uploaded image test-or-confirm. There is
 * nothing to mark: an update is either fully staged, in which case the next
 * boot applies it, or it is not. Answering with the current list is both
 * truthful and what the client expects to parse.
 */
int tinyfota_smp_img_state_write(zcbor_state_t *zsd, zcbor_state_t *zse)
{
	struct zcbor_string hash = {0};
	bool confirm = false;
	size_t decoded;

	struct tinyfota_smp_key keys[] = {
		TINYFOTA_SMP_KEY("hash", TINYFOTA_SMP_BSTR, &hash),
		TINYFOTA_SMP_KEY("confirm", TINYFOTA_SMP_BOOL, &confirm),
	};

	(void)tinyfota_smp_map_decode(zsd, keys, ARRAY_SIZE(keys), &decoded);

	return tinyfota_smp_img_state_read(zse);
}

/* ---- upload --------------------------------------------------------------- */

/**
 * Accept an uploaded image chunk. Decodes offset, total size, data, SHA256 hash, image index, and
 * upgrade flag from the request. Only image 0 is valid. Returns MGMT_ERR_EACCESSDENIED if no update
 * window is open, MGMT_ERR_EINVAL for missing or invalid offset or wrong image index,
 * MGMT_ERR_EBADSTATE for other failures, or MGMT_ERR_EOK on success. Echoes back the next expected
 * offset.
 */
int tinyfota_smp_img_upload(zcbor_state_t *zsd, zcbor_state_t *zse)
{
	struct zcbor_string data = {0};
	struct zcbor_string sha = {0};
	size_t off = SIZE_MAX;
	size_t total = 0;
	uint32_t image = 0;
	bool upgrade = false;
	size_t decoded;
	uint32_t next = 0;
	int rc;
	bool ok;

	struct tinyfota_smp_key keys[] = {
		TINYFOTA_SMP_KEY("image", TINYFOTA_SMP_UINT32, &image),
		TINYFOTA_SMP_KEY("data", TINYFOTA_SMP_BSTR, &data),
		TINYFOTA_SMP_KEY("len", TINYFOTA_SMP_SIZE, &total),
		TINYFOTA_SMP_KEY("off", TINYFOTA_SMP_SIZE, &off),
		TINYFOTA_SMP_KEY("sha", TINYFOTA_SMP_BSTR, &sha),
		TINYFOTA_SMP_KEY("upgrade", TINYFOTA_SMP_BOOL, &upgrade),
	};

	if (tinyfota_smp_map_decode(zsd, keys, ARRAY_SIZE(keys), &decoded) != 0) {
		return TINYFOTA_SMP_RC_EINVAL;
	}
	if (off == SIZE_MAX) {
		return TINYFOTA_SMP_RC_EINVAL;
	}
	/* Only image 0 exists. Saying so beats silently writing another
	 * image's bytes into the one slot there is. */
	if (image != 0U) {
		return TINYFOTA_SMP_RC_EINVAL;
	}

	rc = tinyfota_rx_upload((uint32_t)off, (uint32_t)total, data.value, data.len, &next);
	if (rc == -EACCES) {
		/* The window is shut. This is the one refusal a legitimate user
		 * will meet, so it gets its own code: the client shows "access
		 * denied" and the fix is to open the update window. */
		LOG_WRN("upload refused: no update window is open");
		return TINYFOTA_SMP_RC_EACCESSDENIED;
	}
	if (rc != 0) {
		return TINYFOTA_SMP_RC_EBADSTATE;
	}

	ok = true;
	if (IS_ENABLED(CONFIG_MCUMGR_SMP_LEGACY_RC_BEHAVIOUR)) {
		ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, TINYFOTA_SMP_RC_OK);
	}
	ok = ok && zcbor_tstr_put_lit(zse, "off") && zcbor_size_put(zse, next);

	return ok ? TINYFOTA_SMP_RC_OK : TINYFOTA_SMP_RC_EMSGSIZE;
}

/* ---- erase ---------------------------------------------------------------- */

/**
 * Throw away whatever is staged.
 *
 * Gated on the window like upload is, and for the same reason: this erases
 * flash, and an unauthenticated peer that can erase in a loop is an
 * availability attack on the device.
 */
int tinyfota_smp_img_erase(void)
{
	int rc;

	if (!tinyfota_window_is_open()) {
		return TINYFOTA_SMP_RC_EACCESSDENIED;
	}
	rc = tinyfota_rx_erase(TINYFOTA_OWNER_SMP);

	return (rc == 0) ? TINYFOTA_SMP_RC_OK :
		(rc == -EBUSY ? TINYFOTA_SMP_RC_EBADSTATE : TINYFOTA_SMP_RC_EUNKNOWN);
}

/* ---- reset, gated ---------------------------------------------------------- */

/**
 * Refuse os_mgmt reset unless an update is actually in flight.
 *
 * THIS IS NOT OPTIONAL ON THIS BOARD. MCUMGR_TRANSPORT_BT_PERM_RW leaves the
 * SMP endpoint writable by any unpaired peer in radio range, and group 0
 * command 5 reboots the device. Without this hook, anyone within a few metres
 * could hold the device in a reboot loop -- an availability attack that needs
 * no credential, no pairing and no knowledge of the firmware, and the single
 * strongest argument against putting mcumgr on a device like a lock at all.
 *
 * The gate is the same one the upload uses: the owner has opened an update
 * window, or a verified patch is already staged and waiting to be applied.
 * Outside those two states the device has no reason to accept a remote reboot.
 */
bool tinyfota_smp_reset_allowed(void)
{
	if (tinyfota_window_is_open() || tinyfota_rx_staged()) {
		return true;
	}

	LOG_WRN("reset refused: no update window is open and nothing is staged");
	return false;
}
