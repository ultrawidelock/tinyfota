/* SPDX-License-Identifier: ISC */

/**
 * @file
 * @brief SMP image-management group, so a stock mcumgr client can push a delta.
 *
 * Zephyr's img_mgmt is gated off by single-slot mode, and single-slot is the
 * only way MCUboot fits this part (two slots of ultrawidelock's image want
 * 844 KB of a 512 KB flash), so
 * group 1 is served here instead. A thin adapter, not a reimplementation: every
 * byte still goes through tinyfota_rx_upload(), same signature check, size
 * limits, CRC and window gate as the native transport; only CBOR is new.
 * Clients see one image, one slot, active and confirmed -- never a pending
 * second image, because a staged patch has no honest hash until applied. So the
 * guided "firmware upgrade" wizard is not the target; the supported flow is
 * upload (group 1 cmd 1), reset (group 0 cmd 5), then ~17-31 s of MCUboot
 * applying the patch (core/dfu_applier.c; measured in ultrawidelock on a
 * DWM3001CDK).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>

/* Private to mcumgr by its own comment, but published globally by
 * zephyr_include_directories() in subsys/mgmt/mcumgr/util/CMakeLists.txt, and
 * this is the include line img_mgmt.c itself uses. If it is ever really made
 * private the failure is a missing header at compile time, not a silent one. */
#include <mgmt/mcumgr/util/zcbor_bulk.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <pm_config.h>

#include "tinyfota.h"
#include "tinyfota_rx.h"
#include "tinyfota/tinyfota_port.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(tinyfota, CONFIG_TINYFOTA_LOG_LEVEL);

/* MGMT_GROUP_ID_IMAGE. Spelled out rather than included from
 * img_mgmt.h, which is not compiled in this configuration.
 *
 * Claiming it is safe BY INSPECTION, not by convention: mgmt_register_group()
 * is a bare sys_slist_append() with no duplicate check and no reserved range
 * (zephyr/subsys/mgmt/mcumgr/mgmt/src/mgmt.c), and nothing else in this image
 * registers group 1 because Kconfig refuses to build the only other claimant. */
#define TINYFOTA_SMP_GRP_IMG 1

#define TINYFOTA_SMP_IMG_ID_STATE  0
#define TINYFOTA_SMP_IMG_ID_UPLOAD 1
#define TINYFOTA_SMP_IMG_ID_ERASE  5

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
	const struct image_header *h = (const struct image_header *)PM_MCUBOOT_PAD_ADDRESS;

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
static int state_read(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	bool ok;

	ok = zcbor_tstr_put_lit(zse, "images") && zcbor_list_start_encode(zse, 1) &&
	     encode_slot(zse) && zcbor_list_end_encode(zse, 1) &&
	     zcbor_tstr_put_lit(zse, "splitStatus") && zcbor_int32_put(zse, 0);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/**
 * Accept "set state" and change nothing.
 *
 * A client sends this to mark an uploaded image test-or-confirm. There is
 * nothing to mark: an update is either fully staged, in which case the next
 * boot applies it, or it is not. Answering with the current list is both
 * truthful and what the client expects to parse.
 */
static int state_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	struct zcbor_string hash = {0};
	bool confirm = false;
	size_t decoded;

	struct zcbor_map_decode_key_val keys[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("hash", zcbor_bstr_decode, &hash),
		ZCBOR_MAP_DECODE_KEY_DECODER("confirm", zcbor_bool_decode, &confirm),
	};

	(void)zcbor_map_decode_bulk(zsd, keys, ARRAY_SIZE(keys), &decoded);

	return state_read(ctxt);
}

/* ---- upload --------------------------------------------------------------- */

/**
 * Accept an uploaded image chunk. Decodes offset, total size, data, SHA256 hash, image index, and
 * upgrade flag from the request. Only image 0 is valid. Returns MGMT_ERR_EACCESSDENIED if no update
 * window is open, MGMT_ERR_EINVAL for missing or invalid offset or wrong image index,
 * MGMT_ERR_EBADSTATE for other failures, or MGMT_ERR_EOK on success. Echoes back the next expected
 * offset.
 */
static int upload_write(struct smp_streamer *ctxt)
{
	zcbor_state_t *zsd = ctxt->reader->zs;
	zcbor_state_t *zse = ctxt->writer->zs;
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

	struct zcbor_map_decode_key_val keys[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("image", zcbor_uint32_decode, &image),
		ZCBOR_MAP_DECODE_KEY_DECODER("data", zcbor_bstr_decode, &data),
		ZCBOR_MAP_DECODE_KEY_DECODER("len", zcbor_size_decode, &total),
		ZCBOR_MAP_DECODE_KEY_DECODER("off", zcbor_size_decode, &off),
		ZCBOR_MAP_DECODE_KEY_DECODER("sha", zcbor_bstr_decode, &sha),
		ZCBOR_MAP_DECODE_KEY_DECODER("upgrade", zcbor_bool_decode, &upgrade),
	};

	if (zcbor_map_decode_bulk(zsd, keys, ARRAY_SIZE(keys), &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}
	if (off == SIZE_MAX) {
		return MGMT_ERR_EINVAL;
	}
	/* Only image 0 exists. Saying so beats silently writing another
	 * image's bytes into the one slot there is. */
	if (image != 0U) {
		return MGMT_ERR_EINVAL;
	}

	rc = tinyfota_rx_upload((uint32_t)off, (uint32_t)total, data.value, data.len, &next);
	if (rc == -EACCES) {
		/* The window is shut. This is the one refusal a legitimate user
		 * will meet, so it gets its own code: the client shows "access
		 * denied" and the fix is to open the update window. */
		LOG_WRN("upload refused: no update window is open");
		return MGMT_ERR_EACCESSDENIED;
	}
	if (rc != 0) {
		return MGMT_ERR_EBADSTATE;
	}

	ok = true;
	if (IS_ENABLED(CONFIG_MCUMGR_SMP_LEGACY_RC_BEHAVIOUR)) {
		ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, MGMT_ERR_EOK);
	}
	ok = ok && zcbor_tstr_put_lit(zse, "off") && zcbor_size_put(zse, next);

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/* ---- erase ---------------------------------------------------------------- */

/**
 * Throw away whatever is staged.
 *
 * Gated on the window like upload is, and for the same reason: this erases
 * flash, and an unauthenticated peer that can erase in a loop is an
 * availability attack on the device.
 */
static int erase_write(struct smp_streamer *ctxt)
{
	int rc;

	ARG_UNUSED(ctxt);

	if (!tinyfota_window_is_open()) {
		return MGMT_ERR_EACCESSDENIED;
	}
	rc = tinyfota_rx_erase(TINYFOTA_OWNER_SMP);

	return (rc == 0) ? MGMT_ERR_EOK :
		(rc == -EBUSY ? MGMT_ERR_EBADSTATE : MGMT_ERR_EUNKNOWN);
}

/* ---- reset, gated ---------------------------------------------------------- */

#ifdef CONFIG_MCUMGR_GRP_OS_RESET_HOOK
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
static enum mgmt_cb_return reset_gate(uint32_t event, enum mgmt_cb_return prev_status, int32_t *rc,
				      uint16_t *group, bool *abort_more, void *data,
				      size_t data_size)
{
	ARG_UNUSED(prev_status);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(data_size);

	if (event != MGMT_EVT_OP_OS_MGMT_RESET) {
		return MGMT_CB_OK;
	}
	if (tinyfota_window_is_open() || tinyfota_rx_staged()) {
		return MGMT_CB_OK;
	}

	LOG_WRN("reset refused: no update window is open and nothing is staged");
	*rc = MGMT_ERR_EACCESSDENIED;
	return MGMT_CB_ERROR_RC;
}

static struct mgmt_callback tinyfota_smp_reset_cb = {
	.callback = reset_gate,
	.event_id = MGMT_EVT_OP_OS_MGMT_RESET,
};
#endif /* CONFIG_MCUMGR_GRP_OS_RESET_HOOK */

/* ---- registration --------------------------------------------------------- */

static const struct mgmt_handler tinyfota_smp_img_handlers[] = {
	[TINYFOTA_SMP_IMG_ID_STATE] =
		{
			.mh_read = state_read,
			.mh_write = state_write,
		},
	[TINYFOTA_SMP_IMG_ID_UPLOAD] =
		{
			.mh_read = NULL,
			.mh_write = upload_write,
		},
	/* 2 (file), 3 (corelist) and 4 (coreload) stay NULL: mgmt_find_handler()
	 * treats a NULL pair as "not supported" and the client is told so. */
	[TINYFOTA_SMP_IMG_ID_ERASE] =
		{
			.mh_read = NULL,
			.mh_write = erase_write,
		},
};

static struct mgmt_group tinyfota_smp_img_group = {
	.mg_handlers = (struct mgmt_handler *)tinyfota_smp_img_handlers,
	.mg_handlers_count = ARRAY_SIZE(tinyfota_smp_img_handlers),
	.mg_group_id = TINYFOTA_SMP_GRP_IMG,
};

/**
 * SYS_INIT callback that registers the tinyfota_smp_img group with mcumgr and optionally
 * registers the reset callback if CONFIG_MCUMGR_GRP_OS_RESET_HOOK is enabled.
 */
static void tinyfota_smp_img_init(void)
{
	/* Set here rather than in the initializer above, and not for style: semgrep
	 * parses C without a preprocessor and gives up on an #ifdef between
	 * designated initializers, which silently drops this entire file from the
	 * SAST gate. This file parses attacker-supplied SMP upload frames, so that
	 * is the one place the coverage is worth more than the tidier syntax. The
	 * same #ifdef inside a function body parses fine. Nothing reads the group
	 * before the registration below, so the assignment is equivalent. */
#ifdef CONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME
	tinyfota_smp_img_group.mg_group_name = "img mgmt";
#endif
	mgmt_register_group(&tinyfota_smp_img_group);
#ifdef CONFIG_MCUMGR_GRP_OS_RESET_HOOK
	mgmt_callback_register(&tinyfota_smp_reset_cb);
#endif
}

MCUMGR_HANDLER_DEFINE(tinyfota_smp_img, tinyfota_smp_img_init);
