/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief The update channel on tinyble: SMP over GATT without mcumgr, and the
 *        receiver's own frames.
 *
 * tinyble is a minimal BLE host, not Zephyr's: it owns the GATT table
 * (CONFIG_TINYBLE_TINYFOTA adds the two values in tinyble/tb_dfu.h) and calls
 * the three tb_dfu_* hooks below in its one context, the system work queue,
 * with tb_zephyr_lock() held. Both values feed the same receiver as the
 * Zephyr host's transports, behind the same window, signature check, limits
 * and CRC:
 *
 *   TB_DFU_H_SMP     SMP, what `tinyfota push` and nRF Device Manager speak.
 *                    The image group and the reset gate are smp_group.c,
 *                    shared with the mcumgr build. Here: the 8-byte header,
 *                    a request reassembled from several writes, the os
 *                    group's echo, reset and parameters, and NOT_SUPPORTED
 *                    for everything else, the way mcumgr answers.
 *   TB_DFU_H_NATIVE  tinyfota_rx_frame() (FORMAT.md), owner GATT.
 *
 * FLASH IS WRITTEN FROM THE HOOK, on the system work queue, because tinyble
 * has no other context. The largest single call is the erase of the whole
 * staging partition (32 KB, eight 4 KB pages) when the signed header verifies.
 * Not measured; while it runs nothing else on the queue does, tinyble's HCI
 * processing included, and the controller holds the link.
 *
 * Replies are notifications, sent from a work item on the same queue once
 * the hook has returned, so a Write Request's response goes first. tinyble
 * sends one PDU at a time and says TB_EBUSY until the last one has gone to
 * the controller; the work item then tries again TX_RETRY_MS later. An SMP
 * reply longer than MTU - 3 is split over several notifications, which SMP
 * over GATT allows: the client reassembles by the header's length.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include <tinyble/tb_dfu.h>
#include <tinyble/tb_zephyr.h>
#include <tinyble/tinyble.h>

#include <string.h>

#include "smp_group.h"
#include "tinyfota_rx.h"
#include "tinyfota/tinyfota_port.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(tinyfota, CONFIG_TINYFOTA_LOG_LEVEL);

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* ---- SMP framing ------------------------------------------------------------
 *
 * The header, as mcumgr lays it out (smp_internal.h, little-endian
 * bitfields): op in bits 0-2 of byte 0 and the protocol version in bits 3-4,
 * flags, a big-endian payload length, a big-endian group, the sequence
 * number and the command id.
 */
#define SMP_HDR_LEN 8u

#define SMP_OP_READ      0u
#define SMP_OP_READ_RSP  1u
#define SMP_OP_WRITE     2u
#define SMP_OP_WRITE_RSP 3u

/* Versions 0 (the original protocol) and 1 (SMP version 2) are both served,
 * as mcumgr serves them with CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL. A
 * reply carries the request's version, capped at this. */
#define SMP_VERSION_MAX 1u

#define SMP_GRP_OS         0u
#define SMP_OS_ID_ECHO     0u
#define SMP_OS_ID_RESET    5u
#define SMP_OS_ID_PARAMS   6u

/* mcumgr's CONFIG_MCUMGR_SMP_CBOR_MAX_MAIN_MAP_ENTRIES default. zcbor only
 * counts map entries when ZCBOR_CANONICAL is set, which Zephyr does not set,
 * so maps go out with indefinite length either way. */
#define SMP_MAIN_MAP_MAX 15u

/* mcumgr's CONFIG_MCUMGR_GRP_OS_RESET_MS default: long enough for the reset
 * reply to reach the controller first. */
#define SMP_RESET_MS 250

/* The largest reply other than an echo: the image list, 167 B with a 23
 * character version string (tests/host/test_tinyble.c). An echo that does not
 * fit is refused with EMSGSIZE, as mcumgr refuses one larger than its
 * buffer. */
#define SMP_TX_MAX 192u

#define TX_RETRY_MS 1

/* ATT's Invalid Handle, for a value that is not one of these two. */
#define ATT_ERR_INVALID_HANDLE 0x01u

BUILD_ASSERT(CONFIG_TINYFOTA_TINYBLE_SMP_BUF <= UINT16_MAX);

static struct {
	uint8_t rx[CONFIG_TINYFOTA_TINYBLE_SMP_BUF];
	uint8_t tx[SMP_TX_MAX];
	uint32_t skip;   /**< bytes of a request too large to hold, still to come */
	uint16_t rx_len; /**< bytes of the request in progress */
	uint16_t tx_len; /**< reply bytes; 0 when no reply is owed */
	uint16_t tx_off; /**< reply bytes already notified */
} s_smp;

static struct {
	uint8_t rsp[TINYFOTA_RSP_MAX];
	uint8_t len; /**< 0 when no reply is owed */
} s_native;

static void tx_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(s_tx_work, tx_work_fn);

static void reset_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_INF("SMP reset");
	tinyfota_reboot();
}
static K_WORK_DELAYABLE_DEFINE(s_reset_work, reset_work_fn);

static uint16_t get_be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static uint32_t smp_total(void)
{
	return SMP_HDR_LEN + get_be16(&s_smp.rx[2]);
}

/* ---- the os group: what mcumgr's os_mgmt would answer ------------------------ */

/** Echo, read or write: {"d": text} back as {"r": text}. */
static int os_echo(zcbor_state_t *zsd, zcbor_state_t *zse)
{
	struct zcbor_string d = {(const uint8_t *)"", 0};
	size_t decoded;
	struct tinyfota_smp_key keys[] = {
		TINYFOTA_SMP_KEY("d", TINYFOTA_SMP_TSTR, &d),
	};

	if (tinyfota_smp_map_decode(zsd, keys, ARRAY_SIZE(keys), &decoded) != 0) {
		return TINYFOTA_SMP_RC_EINVAL;
	}
	return (zcbor_tstr_put_lit(zse, "r") && zcbor_tstr_encode(zse, &d)) ?
		       TINYFOTA_SMP_RC_OK : TINYFOTA_SMP_RC_EMSGSIZE;
}

/**
 * Reset, behind the same gate as the mcumgr build's reset hook. The request
 * body is not read: mcumgr's "force" changes nothing the gate looks at, and
 * mcumgr ignores a body that will not decode.
 */
static int os_reset(void)
{
	if (!tinyfota_smp_reset_allowed()) {
		return TINYFOTA_SMP_RC_EACCESSDENIED;
	}
	(void)k_work_schedule(&s_reset_work, K_MSEC(SMP_RESET_MS));
	return TINYFOTA_SMP_RC_OK;
}

/**
 * mcumgr parameters: the largest request this reassembles, and one buffer, so
 * a client that reads them sends one request at a time.
 */
static int os_params(zcbor_state_t *zse)
{
	bool ok = zcbor_tstr_put_lit(zse, "buf_size") &&
		  zcbor_uint32_put(zse, CONFIG_TINYFOTA_TINYBLE_SMP_BUF) &&
		  zcbor_tstr_put_lit(zse, "buf_count") && zcbor_uint32_put(zse, 1);

	return ok ? TINYFOTA_SMP_RC_OK : TINYFOTA_SMP_RC_EMSGSIZE;
}

/* ---- dispatch ---------------------------------------------------------------- */

static int smp_dispatch(uint8_t op, uint16_t group, uint8_t id, zcbor_state_t *zsd,
			zcbor_state_t *zse)
{
	const bool rd = (op == SMP_OP_READ);

	if (group == SMP_GRP_OS) {
		switch (id) {
		case SMP_OS_ID_ECHO:
			return os_echo(zsd, zse);
		case SMP_OS_ID_RESET:
			return rd ? TINYFOTA_SMP_RC_ENOTSUP : os_reset();
		case SMP_OS_ID_PARAMS:
			return rd ? os_params(zse) : TINYFOTA_SMP_RC_ENOTSUP;
		default:
			break;
		}
	} else if (group == TINYFOTA_SMP_GRP_IMG) {
		switch (id) {
		case TINYFOTA_SMP_IMG_ID_STATE:
			return rd ? tinyfota_smp_img_state_read(zse) :
				    tinyfota_smp_img_state_write(zsd, zse);
		case TINYFOTA_SMP_IMG_ID_UPLOAD:
			return rd ? TINYFOTA_SMP_RC_ENOTSUP : tinyfota_smp_img_upload(zsd, zse);
		case TINYFOTA_SMP_IMG_ID_ERASE:
			return rd ? TINYFOTA_SMP_RC_ENOTSUP : tinyfota_smp_img_erase();
		default:
			break;
		}
	}
	return TINYFOTA_SMP_RC_ENOTSUP;
}

/**
 * Answer the request whose header is at the front of s_smp.rx, with
 * @p body_len payload bytes behind it. A refusal is {"rc": status} in place
 * of whatever the handler had written, as mcumgr's smp_build_err_rsp() makes it.
 */
static void smp_answer(uint16_t body_len, int rc)
{
	const uint8_t *req = s_smp.rx;
	const uint8_t op = req[0] & 0x07u;
	const uint8_t ver = MIN((uint8_t)((req[0] >> 3) & 0x03u), (uint8_t)SMP_VERSION_MAX);
	zcbor_state_t zse[2];
	zcbor_state_t zsd[4];
	uint16_t len;

	zcbor_new_encode_state(zse, ARRAY_SIZE(zse), &s_smp.tx[SMP_HDR_LEN],
			       sizeof(s_smp.tx) - SMP_HDR_LEN, 0);

	/* In mcumgr's order: the op first (smp_process_request_packet()), then
	 * the version (smp_handle_single_req()). */
	if (rc == TINYFOTA_SMP_RC_OK && op != SMP_OP_READ && op != SMP_OP_WRITE) {
		rc = TINYFOTA_SMP_RC_ENOTSUP;
	}
	if (rc == TINYFOTA_SMP_RC_OK && ((req[0] >> 3) & 0x03u) > SMP_VERSION_MAX) {
		rc = TINYFOTA_SMP_RC_TOO_NEW;
	}
	if (rc == TINYFOTA_SMP_RC_OK) {
		zcbor_new_decode_state(zsd, ARRAY_SIZE(zsd), &req[SMP_HDR_LEN], body_len, 1, NULL,
				       0);
		if (!zcbor_map_start_encode(zse, SMP_MAIN_MAP_MAX)) {
			rc = TINYFOTA_SMP_RC_EMSGSIZE;
		} else {
			rc = smp_dispatch(op, get_be16(&req[4]), req[7], zsd, zse);
			if (!zcbor_map_end_encode(zse, SMP_MAIN_MAP_MAX) && rc == TINYFOTA_SMP_RC_OK) {
				rc = TINYFOTA_SMP_RC_EMSGSIZE;
			}
		}
	}
	if (rc != TINYFOTA_SMP_RC_OK) {
		zcbor_new_encode_state(zse, ARRAY_SIZE(zse), &s_smp.tx[SMP_HDR_LEN],
				       sizeof(s_smp.tx) - SMP_HDR_LEN, 0);
		/* Six bytes, which always fit. */
		(void)(zcbor_map_start_encode(zse, 1) && zcbor_tstr_put_lit(zse, "rc") &&
		       zcbor_int32_put(zse, rc) && zcbor_map_end_encode(zse, 1));
	}

	len = (uint16_t)(zse->payload_mut - &s_smp.tx[SMP_HDR_LEN]);
	s_smp.tx[0] = (uint8_t)(((op == SMP_OP_READ) ? SMP_OP_READ_RSP : SMP_OP_WRITE_RSP) |
				(ver << 3));
	s_smp.tx[1] = 0;
	put_be16(&s_smp.tx[2], len);
	memcpy(&s_smp.tx[4], &req[4], 4); /* group, sequence number, command id */
	s_smp.tx_len = SMP_HDR_LEN + len;
	s_smp.tx_off = 0;
	(void)k_work_reschedule(&s_tx_work, K_NO_WAIT);
}

/**
 * The header at the front of s_smp.rx is complete, or the whole request is.
 * One request is answered at a time: one arriving while the last reply is
 * still going out is dropped, which os_params() tells clients to expect.
 */
static void smp_complete(int rc)
{
	if (s_smp.tx_len != 0U) {
		LOG_WRN("SMP request while the last reply is still going out: dropped");
		return;
	}
	smp_answer((uint16_t)(s_smp.rx_len - SMP_HDR_LEN), rc);
}

static void smp_rx(const uint8_t *data, uint16_t len)
{
	while (len > 0U) {
		uint16_t n;

		if (s_smp.skip > 0U) {
			n = (uint16_t)MIN((uint32_t)len, s_smp.skip);
			s_smp.skip -= n;
		} else {
			const uint32_t want = (s_smp.rx_len < SMP_HDR_LEN) ? SMP_HDR_LEN : smp_total();

			n = (uint16_t)MIN((uint32_t)len, want - s_smp.rx_len);
			memcpy(&s_smp.rx[s_smp.rx_len], data, n);
			s_smp.rx_len += n;

			if (s_smp.rx_len == SMP_HDR_LEN && smp_total() > sizeof(s_smp.rx)) {
				/* Too large to reassemble. Refused by its header, and
				 * the rest of it is let through unread. */
				LOG_WRN("SMP request of %u B refused, the buffer holds %u B",
					(unsigned)smp_total(), (unsigned)sizeof(s_smp.rx));
				smp_complete(TINYFOTA_SMP_RC_ENOMEM);
				s_smp.skip = smp_total() - SMP_HDR_LEN;
				s_smp.rx_len = 0;
			} else if (s_smp.rx_len >= SMP_HDR_LEN && s_smp.rx_len == smp_total()) {
				smp_complete(TINYFOTA_SMP_RC_OK);
				s_smp.rx_len = 0;
			}
		}
		data += n;
		len -= n;
	}
}

/* ---- the native frames ------------------------------------------------------- */

/* One reply slot. The host waits for each reply before it sends more; one
 * that does not gets the reply to its latest frame, whose next offset covers
 * the frames before it. */
static void native_rx(const uint8_t *data, uint16_t len)
{
	size_t rsp_len = 0;

	(void)tinyfota_rx_frame(TINYFOTA_OWNER_GATT, data, len, s_native.rsp, &rsp_len);
	s_native.len = (uint8_t)rsp_len;
	(void)k_work_reschedule(&s_tx_work, K_NO_WAIT);
}

/* ---- notifications ----------------------------------------------------------- */

static void smp_tx_drop(void)
{
	s_smp.tx_len = 0;
	s_smp.tx_off = 0;
}

static void tx_work_fn(struct k_work *work)
{
	int rc = 0;

	ARG_UNUSED(work);
	tb_zephyr_lock();
	if (s_native.len > 0U) {
		rc = tb_notify(TB_DFU_H_NATIVE, s_native.rsp, s_native.len);
		if (rc != TB_EBUSY) {
			/* Sent, or it cannot be: no link, or notifications off. */
			s_native.len = 0;
		}
	}
	while (rc != TB_EBUSY && s_smp.tx_len != 0U) {
		const uint16_t n = MIN((uint16_t)(s_smp.tx_len - s_smp.tx_off),
				       (uint16_t)(tb_mtu() - 3u));

		rc = tb_notify(TB_DFU_H_SMP, &s_smp.tx[s_smp.tx_off], n);
		if (rc == 0) {
			s_smp.tx_off += n;
			if (s_smp.tx_off == s_smp.tx_len) {
				smp_tx_drop();
			}
		} else if (rc != TB_EBUSY) {
			LOG_WRN("SMP reply dropped (%d)", rc);
			smp_tx_drop();
		}
	}
	tb_zephyr_unlock();
	if (rc == TB_EBUSY) {
		(void)k_work_reschedule(&s_tx_work, K_MSEC(TX_RETRY_MS));
	}
}

/* ---- tinyble's hooks --------------------------------------------------------- */

uint8_t tb_dfu_on_write(uint16_t handle, const uint8_t *data, uint16_t len)
{
	switch (handle) {
	case TB_DFU_H_SMP:
		smp_rx(data, len);
		return 0;
	case TB_DFU_H_NATIVE:
		native_rx(data, len);
		return 0;
	default:
		return ATT_ERR_INVALID_HANDLE;
	}
}

void tb_dfu_on_ccc(uint16_t handle, uint16_t value)
{
	/* Notifications turned off: a reply owed there cannot be delivered. */
	if ((value & 1U) != 0U) {
		return;
	}
	if (handle == TB_DFU_H_SMP) {
		smp_tx_drop();
	} else if (handle == TB_DFU_H_NATIVE) {
		s_native.len = 0;
	}
}

void tb_dfu_on_disconnect(uint8_t reason)
{
	ARG_UNUSED(reason);
	(void)k_work_cancel_delayable(&s_tx_work);
	s_smp.rx_len = 0;
	s_smp.skip = 0;
	smp_tx_drop();
	s_native.len = 0;
	/* The same release as the Zephyr host's GATT transport, for both owners.
	 * The mcumgr build keeps an SMP claim across a disconnect, so a client
	 * can resume; here a client that reconnects starts again at 0. */
	tinyfota_rx_reset(TINYFOTA_OWNER_SMP);
	tinyfota_rx_reset(TINYFOTA_OWNER_GATT);
}
