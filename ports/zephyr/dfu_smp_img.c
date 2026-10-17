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
 *
 * The commands themselves are in smp_group.c, shared with the SMP server on
 * tinyble (dfu_tinyble.c); this file registers them with mcumgr.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>

#include "smp_group.h"

/* The shared commands return these values to mcumgr as they are. */
BUILD_ASSERT((int)TINYFOTA_SMP_RC_OK == (int)MGMT_ERR_EOK);
BUILD_ASSERT((int)TINYFOTA_SMP_RC_EUNKNOWN == (int)MGMT_ERR_EUNKNOWN);
BUILD_ASSERT((int)TINYFOTA_SMP_RC_EINVAL == (int)MGMT_ERR_EINVAL);
BUILD_ASSERT((int)TINYFOTA_SMP_RC_EBADSTATE == (int)MGMT_ERR_EBADSTATE);
BUILD_ASSERT((int)TINYFOTA_SMP_RC_EMSGSIZE == (int)MGMT_ERR_EMSGSIZE);
BUILD_ASSERT((int)TINYFOTA_SMP_RC_EACCESSDENIED == (int)MGMT_ERR_EACCESSDENIED);

/* MGMT_GROUP_ID_IMAGE (TINYFOTA_SMP_GRP_IMG, smp_group.h). Spelled out rather
 * than included from img_mgmt.h, which is not compiled in this configuration.
 *
 * Claiming it is safe BY INSPECTION, not by convention: mgmt_register_group()
 * is a bare sys_slist_append() with no duplicate check and no reserved range
 * (zephyr/subsys/mgmt/mcumgr/mgmt/src/mgmt.c), and nothing else in this image
 * registers group 1 because Kconfig refuses to build the only other claimant. */

/* ---- the commands, on mcumgr's streamer ------------------------------------ */

static int state_read(struct smp_streamer *ctxt)
{
	return tinyfota_smp_img_state_read(ctxt->writer->zs);
}

static int state_write(struct smp_streamer *ctxt)
{
	return tinyfota_smp_img_state_write(ctxt->reader->zs, ctxt->writer->zs);
}

static int upload_write(struct smp_streamer *ctxt)
{
	return tinyfota_smp_img_upload(ctxt->reader->zs, ctxt->writer->zs);
}

static int erase_write(struct smp_streamer *ctxt)
{
	ARG_UNUSED(ctxt);
	return tinyfota_smp_img_erase();
}

/* ---- reset, gated ---------------------------------------------------------- */

#ifdef CONFIG_MCUMGR_GRP_OS_RESET_HOOK
/**
 * Refuse os_mgmt reset unless an update is actually in flight
 * (tinyfota_smp_reset_allowed() in smp_group.c says why this is not optional).
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
	if (tinyfota_smp_reset_allowed()) {
		return MGMT_CB_OK;
	}

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
