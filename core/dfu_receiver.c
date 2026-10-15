/* SPDX-License-Identifier: ISC */

/**
 * @file
 * @brief Application half of the delta update: receive, verify, stage, reboot.
 *
 * Never applies anything. The patch is written into `patch_staging` and the
 * board is restarted; MCUboot does the work, because the application executes
 * from the slot the patch rewrites (see src/dfu_applier.c).
 *
 * WHAT ARRIVES, in order, as one byte stream over whatever transport:
 *
 *     0   32   struct tinyfota_hdr
 *    32   64   ECDSA-P256 signature, raw r||s, over those 32 bytes
 *    96   ..   the patch
 *
 * The header is written to flash LAST, after the patch's CRC checks out, so a
 * cut-off transfer leaves no valid magic and the next boot ignores it. The
 * signature is checked HERE, not in the bootloader (this image already links
 * PSA ECDSA-P256; MCUboot is the flash-starved one), and the floor sits under
 * both: CONFIG_BOOT_VALIDATE_SLOT0 re-verifies the RESULT before booting, so a
 * forged header can only destroy the installed image, never install code.
 */

#include <errno.h>
#include <string.h>

#include "dfu_crc.h"
#include "tinyfota.h"
#include "tinyfota_rx.h"
#include "tinyfota/tinyfota_port.h"

LOG_MODULE_REGISTER(tinyfota, CONFIG_TINYFOTA_LOG_LEVEL);

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/** Generated at build time from the MCUboot signing key. 0x04 || X || Y. */
extern const uint8_t tinyfota_pubkey[65];

/** Bytes of preamble ahead of the patch: the header and its signature. */
#define HEAD_LEN (TINYFOTA_HDR_LEN + TINYFOTA_SIG_LEN)

/** Flash write staging. Multiple of the 4-byte write block. */
#define WBUF_SZ 64

static struct {
	bool active;
	bool erased;
	bool staged;
	enum tinyfota_owner owner;
	uint32_t transfer_id;
	uint32_t total; /**< wire bytes the host promised */
	uint32_t got;   /**< wire bytes consumed so far */
	uint32_t patch_crc;
	uint32_t wpos; /**< next staging offset for patch data */
	uint8_t head[HEAD_LEN];
	uint8_t wbuf[WBUF_SZ];
	size_t wlen;
} s_rx;

static const struct tinyfota_flash_area *s_fa;
/* The receiver lock is the port's: tinyfota_lock()/tinyfota_unlock(). */
#ifdef CONFIG_TINYFOTA_SMP_IMG
/* SMP client-space cursor and optional MCUboot wrapper geometry. */
static uint32_t s_cli_off;
static uint32_t s_skip;
static uint32_t s_inner;
#endif

/* Replying before rebooting is the whole reason this is deferred: a reboot
 * inside the frame handler drops the acknowledgement the host is waiting for,
 * and the host cannot then tell success from a dead board. */
static void reboot_fn(void)
{
	LOG_INF("update staged, restarting into the bootloader");
	tinyfota_reboot();
}

/* ---- window --------------------------------------------------------------- */

static void window_expire(void);
static bool s_open;
static int64_t s_window_deadline_ms;
static tinyfota_window_cb s_window_cb;
/* NULL on every port whose bootloader reads the staging partition by itself,
 * which is all of them except ESP-IDF. See tinyfota_rx.h. */
static tinyfota_commit_cb s_commit_cb;

/* Both delayable items are bound on the first window call, which every route
 * into the receiver goes through -- frames and SMP uploads are refused while
 * the window has never opened. */
static void works_bind(void)
{
	static bool bound;

	if (!bound) {
		tinyfota_timer_init(TINYFOTA_TIMER_REBOOT, reboot_fn);
		tinyfota_timer_init(TINYFOTA_TIMER_WINDOW, window_expire);
		bound = true;
	}
}

/**
 * Register a callback to be invoked when the update window opens or closes.
 */
void tinyfota_set_window_cb(tinyfota_window_cb cb)
{
	works_bind();
	tinyfota_lock();
	s_window_cb = cb;
	tinyfota_unlock();
}

/**
 * Register the port's commit hook. See tinyfota_rx.h for what it is
 * for and when it runs.
 */
void tinyfota_set_commit_cb(tinyfota_commit_cb cb)
{
	works_bind();
	tinyfota_lock();
	s_commit_cb = cb;
	tinyfota_unlock();
}

/* One place, so that every route in -- the button, an app command, the bench
 * SWD write -- reaches the indicator without knowing it exists. */
static void window_notify(bool open)
{
	tinyfota_window_cb cb;

	tinyfota_lock();
	cb = s_window_cb;
	tinyfota_unlock();
	if (cb != NULL) {
		cb(open);
	}
}

/**
 * Mark the update window closed, reset RX state, and notify all listeners (typically the UI) that
 * the window is no longer open.
 */
static void rx_state_reset_locked(void)
{
	memset(&s_rx, 0, sizeof(s_rx));
}

#ifdef CONFIG_TINYFOTA_SMP_IMG
static void smp_cursor_reset_locked(void)
{
	s_cli_off = 0u;
	s_skip = 0u;
	s_inner = 0u;
}
#endif

static void rx_reset_all_locked(void)
{
	rx_state_reset_locked();
#ifdef CONFIG_TINYFOTA_SMP_IMG
	smp_cursor_reset_locked();
#endif
}

static void window_expire(void)
{
	int64_t remaining_ms;

	tinyfota_lock();
	if (!s_open) {
		tinyfota_unlock();
		return;
	}
#if defined(TINYFOTA_PORT_HOST)
	/* The deterministic host scheduler has its own virtual clock and removes a
	 * rescheduled item atomically, so an old callback cannot overlap here. */
	remaining_ms = 0;
#else
	remaining_ms = s_window_deadline_ms - tinyfota_uptime_ms();
#endif
	if (remaining_ms > 0) {
		/* A callback from the previous deadline can already be running when
		 * the owner extends the window. It must not close the new window. */
		(void)tinyfota_timer_reschedule(
			TINYFOTA_TIMER_WINDOW, remaining_ms > INT32_MAX ? INT32_MAX : (int32_t)remaining_ms);
		tinyfota_unlock();
		return;
	}
	LOG_INF("update window closed");
	s_open = false;
	s_window_deadline_ms = 0;
	rx_reset_all_locked();
	tinyfota_unlock();
	window_notify(false);
}

/**
 * Open the update window for the given duration in milliseconds. Reschedule the close timer and
 * notify all window listeners.
 */
void tinyfota_window_open(uint32_t duration_ms)
{
	works_bind();
	tinyfota_lock();
	s_open = true;
	s_window_deadline_ms = tinyfota_uptime_ms() + duration_ms;
	(void)tinyfota_timer_reschedule(
		TINYFOTA_TIMER_WINDOW, duration_ms > INT32_MAX ? INT32_MAX : (int32_t)duration_ms);
	tinyfota_unlock();
	LOG_INF("update window open for %u ms", (unsigned)duration_ms);
	window_notify(true);
}

/**
 * Cancel the update window timer, mark it closed, reset RX state, and notify all listeners that the
 * window is no longer open.
 */
void tinyfota_window_close(void)
{
	works_bind();
	(void)tinyfota_timer_cancel(TINYFOTA_TIMER_WINDOW);
	tinyfota_lock();
	s_open = false;
	s_window_deadline_ms = 0;
	rx_reset_all_locked();
	tinyfota_unlock();
	window_notify(false);
}

/**
 * Return true if the update window is currently open.
 */
bool tinyfota_window_is_open(void)
{
	bool open;

	works_bind();
	tinyfota_lock();
	open = s_open;
	tinyfota_unlock();
	return open;
}

/* ---- staging flash -------------------------------------------------------- */

/**
 * Open the staging flash area if not already open. Return 0 on success or if already open; nonzero
 * on error.
 */
static int staging_open(void)
{
	if (s_fa != NULL) {
		return 0;
	}
	return tinyfota_flash_open(TINYFOTA_FLASH_AREA_STAGING, &s_fa);
}

/** Room for patch bytes, after the header page and the step-log page. */
static uint32_t patch_max(void)
{
	return (uint32_t)tinyfota_flash_size(s_fa) - TINYFOTA_PATCH_OFFSET;
}

/**
 * Flush buffered patch data to the staging flash area, padding to 4-byte alignment if final. Return
 * 0 on success, -1 on write error. Updates write position and shifts remaining bytes.
 */
static int wbuf_flush(bool final)
{
	size_t n = s_rx.wlen & ~(size_t)3;

	if (final) {
		while (s_rx.wlen & 3U) {
			s_rx.wbuf[s_rx.wlen++] = 0xff;
		}
		n = s_rx.wlen;
	}
	if (n == 0U) {
		return 0;
	}
	if (tinyfota_flash_write(s_fa, s_rx.wpos, s_rx.wbuf, n) != 0) {
		return -1;
	}

	s_rx.wpos += (uint32_t)n;
	s_rx.wlen -= n;
	if (s_rx.wlen > 0U) {
		memmove(s_rx.wbuf, s_rx.wbuf + n, s_rx.wlen);
	}
	return 0;
}

/**
 * Buffer patch data, updating the running CRC32, and flush to flash when the buffer is full. Return
 * 0 on success or nonzero on flush failure.
 */
static int patch_write(const uint8_t *data, size_t len)
{
	s_rx.patch_crc = tinyfota_crc32_update(s_rx.patch_crc, data, len);

	while (len > 0U) {
		size_t n = MIN(len, WBUF_SZ - s_rx.wlen);

		memcpy(s_rx.wbuf + s_rx.wlen, data, n);
		s_rx.wlen += n;
		data += n;
		len -= n;

		if (s_rx.wlen == WBUF_SZ && wbuf_flush(false) != 0) {
			return -1;
		}
	}
	return 0;
}

/* ---- authenticity --------------------------------------------------------- */

/**
 * Verify the DFU header signature using ECDSA-SHA256 with the built-in public key. Return true if
 * the signature is valid.
 */
static bool head_verifies(void)
{
	/* The crypto provider's explicit, idempotent init happens inside the
	 * port's tinyfota_ecdsa_p256_verify(), so the receiver remains a
	 * self-contained consumer and fails closed in a smaller image. */
	return tinyfota_ecdsa_p256_verify(
		       tinyfota_pubkey, s_rx.head, TINYFOTA_HDR_LEN,
		       s_rx.head + TINYFOTA_HDR_LEN) == 0;
}

/* ---- the base image ------------------------------------------------------- */

/**
 * Is the primary slot the image the patch was built from?
 *
 * The applier checks the same CRC before it touches the slot, but only after
 * a reboot, by which time the staging partition has already been erased and
 * rewritten. Checking it here, after the signature and before the first
 * erase, turns a patch for another image into an immediate refusal the host
 * can report, with the staging partition left as it was. The header is
 * signed, so from_len and from_crc32 are the signer's claim, not the peer's.
 *
 * Returns TINYFOTA_ERR_OK, TINYFOTA_ERR_BASE, or TINYFOTA_ERR_FLASH when the
 * slot cannot be read at all -- a port fault, not a wrong patch.
 */
static enum tinyfota_err base_check(void)
{
	const struct tinyfota_flash_area *primary;
	struct tinyfota_hdr hdr;
	uint8_t buf[64];
	uint32_t off = 0;
	uint32_t crc = 0;
	enum tinyfota_err e = TINYFOTA_ERR_BASE;

	memcpy(&hdr, s_rx.head, sizeof(hdr));
	if (tinyfota_flash_open(TINYFOTA_FLASH_AREA_PRIMARY, &primary) != 0) {
		return TINYFOTA_ERR_FLASH;
	}
	if (hdr.from_len <= tinyfota_flash_size(primary)) {
		while (off < hdr.from_len) {
			size_t n = MIN(sizeof(buf), (size_t)(hdr.from_len - off));

			if (tinyfota_flash_read(primary, off, buf, n) != 0) {
				break;
			}
			crc = tinyfota_crc32_update(crc, buf, n);
			off += (uint32_t)n;
		}
		if (off != hdr.from_len) {
			e = TINYFOTA_ERR_FLASH;
		} else if (crc == hdr.from_crc32) {
			e = (enum tinyfota_err)0;
		}
	}
	tinyfota_flash_close(primary);
	return e;
}

/* ---- frame handling ------------------------------------------------------- */

/**
 * Reset the receiver state to empty: clear the RX struct.
 */
void tinyfota_rx_reset_all(void)
{
	works_bind();
	tinyfota_lock();
	rx_reset_all_locked();
	tinyfota_unlock();
}

void tinyfota_rx_reset(enum tinyfota_owner owner)
{
	works_bind();
	tinyfota_lock();
	if (owner != TINYFOTA_OWNER_NONE && s_rx.owner == owner) {
		rx_reset_all_locked();
	}
	tinyfota_unlock();
}

static int rx_erase_locked(enum tinyfota_owner owner)
{
	if (!s_open) {
		return -EACCES;
	}
	if (s_rx.owner != TINYFOTA_OWNER_NONE && s_rx.owner != owner) {
		return -EBUSY;
	}
	if (staging_open() != 0 ||
	    tinyfota_flash_erase(s_fa, 0, tinyfota_flash_size(s_fa)) != 0) {
		return -EIO;
	}
	rx_reset_all_locked();
	return 0;
}

int tinyfota_rx_erase(enum tinyfota_owner owner)
{
	int rc;

	works_bind();
	tinyfota_lock();
	rc = rx_erase_locked(owner);
	tinyfota_unlock();
	return rc;
}

/**
 * Write a TINYFOTA_RSP_OK response: set opcode to OK, append the byte count received as
	 * little-endian 32-bit. Return 9 (response size).
 */
static size_t reply_ok(uint8_t *rsp, uint32_t transfer_id)
{
	rsp[0] = TINYFOTA_RSP_OK;
	tinyfota_put_le32(transfer_id, &rsp[1]);
	tinyfota_put_le32(s_rx.got, &rsp[5]);
	return 9u;
}

/**
 * Encode a two-byte error response with the given error code. Return the
 * response length (2 bytes).
 */
static size_t reply_err(uint8_t *rsp, enum tinyfota_err code)
{
	rsp[0] = TINYFOTA_RSP_ERR;
	rsp[1] = (uint8_t)code;
	return 2u;
}

/* The three cores below carry the whole state machine, and they are shared:
 * the framed protocol wraps them in opcodes, and the SMP image group
 * (src/dfu_smp_img.c) drives the same three from CBOR. Neither transport holds
 * any transfer state of its own, so there is one place where a patch can be
 * accepted and one set of checks in front of it. TINYFOTA_ERR_OK (0) means
 * accepted; every other value is refused. */
#define TINYFOTA_ERR_OK ((enum tinyfota_err)0)

/**
 * Validate the patch size and prepare the receiver
 * to accept upload: initialize RX state with the write position at the patch offset and mark
 * reception active. Return an error code.
 */
static enum tinyfota_err begin_at(enum tinyfota_owner owner,
					    uint32_t transfer_id, uint32_t total)
{
	if (owner == TINYFOTA_OWNER_NONE || transfer_id == 0U) {
		return TINYFOTA_ERR_MALFORMED;
	}
	/*
	 * THIS CLAIM IS TAKEN BEFORE ANYTHING IS AUTHENTICATED, and that is an
	 * accepted risk rather than an oversight. The signature over the header is
	 * only checked once HEAD_LEN bytes have arrived, so whoever sends BEGIN
	 * first holds the receiver until they disconnect or the window shuts.
	 *
	 * Weighed and kept, on four counts:
	 *
	 *   - The window is owner-gated. s_open is false until the update button
	 *     or another owner gesture opens it, so there is nothing to claim
	 *     outside a gesture the owner just made.
	 *   - It cannot install anything. head_verifies() still checks the
	 *     ECDSA-P256 signature, and commit_now() re-checks magic, ABI, CRCs
	 *     and lengths. This is denial of an update, never a forged one.
	 *   - CONFIG_BT_MAX_CONN=1. A peer that can send BEGIN has already taken
	 *     the board's only connection slot, which denies the flasher, the
	 *     phone and everything else regardless of what DFU does. The claim
	 *     adds no exposure the connection slot has not already given away.
	 *   - The obvious fix is barred. Requiring an encrypted link would mean
	 *     BT_GATT_PERM_WRITE_ENCRYPT and therefore pairing, and the device
	 *     this was built for deliberately never asks a phone to pair.
	 *
	 * The claim is released on transport disconnect via
	 * tinyfota_rx_reset(), and window_expire() clears everything when
	 * the window shuts, so it cannot outlive either.
	 *
	 * REVISIT IF CONFIG_BT_MAX_CONN EVER EXCEEDS 1. That is the load-bearing
	 * leg above: with two connection slots a second peer can claim the
	 * receiver without denying the first, and the argument stops holding. The
	 * fix that needs no pairing is to preempt a claim that has taken no DATA
	 * bytes after an idle timeout.
	 *
	 * tinyfota's sample reserves a second connection for updates, so that leg
	 * does NOT hold there.
	 */
	if (s_rx.owner != TINYFOTA_OWNER_NONE) {
		if (s_rx.owner == owner && s_rx.transfer_id == transfer_id && s_rx.total == total) {
			return TINYFOTA_ERR_OK;
		}
		return TINYFOTA_ERR_BUSY;
	}
	if (staging_open() != 0) {
		return TINYFOTA_ERR_FLASH;
	}
	if (total <= HEAD_LEN || (total - HEAD_LEN) > patch_max()) {
		return TINYFOTA_ERR_SIZE;
	}

	rx_state_reset_locked();
	s_rx.active = true;
	s_rx.owner = owner;
	s_rx.transfer_id = transfer_id;
	s_rx.total = total;
	s_rx.wpos = TINYFOTA_PATCH_OFFSET;
	LOG_INF("update begun, %u B", (unsigned)total);
	return TINYFOTA_ERR_OK;
}

static enum tinyfota_err feed_bytes(const uint8_t *p, size_t n)
{
	if (!s_rx.active) {
		return TINYFOTA_ERR_SEQUENCE;
	}
	if (s_rx.got + n > s_rx.total) {
		return TINYFOTA_ERR_SIZE;
	}

	/* Preamble first, then everything else is patch. */
	if (s_rx.got < HEAD_LEN) {
		size_t take = MIN(n, HEAD_LEN - s_rx.got);

		memcpy(s_rx.head + s_rx.got, p, take);
		s_rx.got += take;
		p += take;
		n -= take;

		if (s_rx.got == HEAD_LEN) {
			if (!head_verifies()) {
				LOG_WRN("rejected: header signature did not verify");
				return TINYFOTA_ERR_AUTH;
			}
			{
				enum tinyfota_err base = base_check();

				if (base != (enum tinyfota_err)0) {
					LOG_WRN("rejected: built for a different image than this one");
					return base;
				}
			}

			/* Authenticate before the first destructive operation. A random
			 * nearby peer can no longer consume an erase cycle with BEGIN. */
			if (tinyfota_flash_erase(s_fa, 0, tinyfota_flash_size(s_fa)) != 0) {
				return TINYFOTA_ERR_FLASH;
			}
			s_rx.erased = true;
		}
	}

	if (n > 0U) {
		if (patch_write(p, n) != 0) {
			return TINYFOTA_ERR_FLASH;
		}
		s_rx.got += n;
	}

	return TINYFOTA_ERR_OK;
}

/* @p reboot is false only for SMP, where the host sends its own reset command
 * afterwards and a board that restarted on its own would look like a failure. */
static enum tinyfota_err commit_now(bool reboot)
{
	struct tinyfota_hdr hdr;

	if (s_rx.staged) {
		/* A COMMIT whose ack was lost gets retried. The header is already
		 * written, so there is nothing to re-validate -- but returning OK
		 * without re-arming the reboot leaves the host waiting forever for
		 * an update it has in fact delivered. Scheduling is a no-op while
		 * the work is armed, so the original deadline still stands and a
		 * host repeating COMMIT cannot push the reboot out. */
		if (reboot) {
			(void)tinyfota_timer_schedule(TINYFOTA_TIMER_REBOOT, 500);
		}
		return TINYFOTA_ERR_OK;
	}
	if (!s_rx.active || !s_rx.erased || s_rx.got != s_rx.total) {
		return TINYFOTA_ERR_SEQUENCE;
	}
	if (wbuf_flush(true) != 0) {
		return TINYFOTA_ERR_FLASH;
	}

	memcpy(&hdr, s_rx.head, sizeof(hdr));

	if (hdr.magic != TINYFOTA_MAGIC || hdr.abi_version != TINYFOTA_ABI_VERSION ||
	    hdr.hdr_crc32 != tinyfota_crc32(s_rx.head, TINYFOTA_HDR_CRC_LEN) ||
	    hdr.patch_len != (s_rx.total - HEAD_LEN) || hdr.patch_crc32 != s_rx.patch_crc) {
		LOG_WRN("rejected: staged bytes do not match the header");
		return TINYFOTA_ERR_INTEGRITY;
	}

	/* The header goes in LAST. Until this write lands there is no magic in
	 * the staging partition and the bootloader has nothing to act on, so an
	 * interrupted transfer is indistinguishable from no transfer. */
	if (tinyfota_flash_write(s_fa, TINYFOTA_HDR_OFFSET, s_rx.head,
			    TINYFOTA_HDR_LEN) != 0) {
		return TINYFOTA_ERR_FLASH;
	}

	/* The port's last chance to refuse, and on a port with no bootloader of
	 * its own its only chance to ARM one. Failing here must not leave
	 * s_rx.staged set: a board that says "committed" and then boots the old
	 * image is worse than one that says it failed, because the host records
	 * the update as delivered and builds the next one from the wrong base. */
	if (s_commit_cb != NULL) {
		if (s_commit_cb(&hdr) != 0) {
			LOG_WRN("commit refused by the port");
			return TINYFOTA_ERR_FLASH;
		}
	}

	s_rx.active = false;
	s_rx.staged = true;
	if (reboot) {
		(void)tinyfota_timer_schedule(TINYFOTA_TIMER_REBOOT, 500);
	}
	return TINYFOTA_ERR_OK;
}

static size_t do_begin(enum tinyfota_owner owner, const uint8_t *frame, size_t len,
		       uint8_t *rsp)
{
	enum tinyfota_err e;
	uint32_t transfer_id;

	if (len != 9u) {
		return reply_err(rsp, TINYFOTA_ERR_MALFORMED);
	}
	transfer_id = tinyfota_get_le32(&frame[1]);
	e = begin_at(owner, transfer_id, tinyfota_get_le32(&frame[5]));
	return (e == TINYFOTA_ERR_OK) ? reply_ok(rsp, transfer_id) : reply_err(rsp, e);
}

static size_t do_data(enum tinyfota_owner owner, const uint8_t *frame, size_t len,
		      uint8_t *rsp)
{
	enum tinyfota_err e;
	uint32_t transfer_id;
	uint32_t offset;

	if (len < 9u) {
		return reply_err(rsp, TINYFOTA_ERR_MALFORMED);
	}
	transfer_id = tinyfota_get_le32(&frame[1]);
	offset = tinyfota_get_le32(&frame[5]);
	if (s_rx.owner != owner || s_rx.transfer_id != transfer_id) {
		return reply_err(rsp, s_rx.owner == TINYFOTA_OWNER_NONE ?
					       TINYFOTA_ERR_SEQUENCE : TINYFOTA_ERR_BUSY);
	}
	/* Any stale or future offset is a non-destructive resynchronisation. This
	 * makes a retry after a lost GATT notification or CoC SDU idempotent. */
	if (offset != s_rx.got) {
		return reply_ok(rsp, transfer_id);
	}
	e = feed_bytes(&frame[9], len - 9u);
	if (e != TINYFOTA_ERR_OK) {
		rx_reset_all_locked();
		return reply_err(rsp, e);
	}
	return reply_ok(rsp, transfer_id);
}

static size_t do_commit(enum tinyfota_owner owner, const uint8_t *frame, size_t len,
			uint8_t *rsp)
{
	enum tinyfota_err e;
	uint32_t transfer_id;

	if (len != 5u) {
		return reply_err(rsp, TINYFOTA_ERR_MALFORMED);
	}
	transfer_id = tinyfota_get_le32(&frame[1]);
	if (s_rx.owner != owner || s_rx.transfer_id != transfer_id) {
		return reply_err(rsp, s_rx.owner == TINYFOTA_OWNER_NONE ?
					       TINYFOTA_ERR_SEQUENCE : TINYFOTA_ERR_BUSY);
	}
	e = commit_now(true);
	if (e != TINYFOTA_ERR_OK) {
		rx_reset_all_locked();
		return reply_err(rsp, e);
	}
	return reply_ok(rsp, transfer_id);
}

int tinyfota_rx_frame(enum tinyfota_owner owner, const uint8_t *frame, size_t len,
			      uint8_t *rsp, size_t *rsp_len)
{
	works_bind();
	tinyfota_lock();
	if (len < 1u) {
		*rsp_len = reply_err(rsp, TINYFOTA_ERR_MALFORMED);
		tinyfota_unlock();
		return 0;
	}
	if (!s_open) {
		*rsp_len = reply_err(rsp, TINYFOTA_ERR_CLOSED);
		tinyfota_unlock();
		return 0;
	}

	switch (frame[0]) {
	case TINYFOTA_OP_BEGIN:
		*rsp_len = do_begin(owner, frame, len, rsp);
		break;
	case TINYFOTA_OP_DATA:
		*rsp_len = do_data(owner, frame, len, rsp);
		break;
	case TINYFOTA_OP_COMMIT:
		*rsp_len = do_commit(owner, frame, len, rsp);
		break;
	case TINYFOTA_OP_ABORT:
		if (len != 5u || s_rx.owner != owner || s_rx.transfer_id != tinyfota_get_le32(&frame[1])) {
			*rsp_len = reply_err(rsp, TINYFOTA_ERR_SEQUENCE);
			break;
		}
		{
			uint32_t transfer_id = s_rx.transfer_id;
			int rc = rx_erase_locked(owner);

			*rsp_len = (rc == 0) ? reply_ok(rsp, transfer_id) :
						 reply_err(rsp, TINYFOTA_ERR_FLASH);
		}
		break;
	default:
		*rsp_len = reply_err(rsp, TINYFOTA_ERR_MALFORMED);
		break;
	}

	tinyfota_unlock();
	return 0;
}

#ifdef CONFIG_TINYFOTA_SMP_IMG

/* MCUboot's image magic and the two header fields needed to measure a wrapper.
 * Also spelled out in src/dfu_smp_img.c, which walks the running image's TLVs;
 * this is MCUboot's fixed on-disk format and cannot drift. */
#define MCUBOOT_IMAGE_MAGIC 0x96f3b83du
#define MCUBOOT_HDR_MIN     16u

/* The CLIENT's cursor, which stops matching the patch cursor the moment a file
 * is wrapped. Everything reported back to the host is in this space; s_rx.got
 * remains the cursor into the patch itself. */
static int rx_upload_locked(uint32_t off, uint32_t total, const uint8_t *data, size_t len,
			    uint32_t *next)
{
	enum tinyfota_err e;

	if (!s_open) {
		return -EACCES;
	}
	if (s_rx.owner != TINYFOTA_OWNER_NONE &&
	    s_rx.owner != TINYFOTA_OWNER_SMP) {
		return -EBUSY;
	}
	if (off == 0U && s_rx.owner == TINYFOTA_OWNER_SMP && s_cli_off > 0U) {
		*next = s_cli_off;
		return 0;
	}

	if (off == 0U) {
		/* A restarted upload is normal, not an error: mcumgr clients
			 * rewind to 0 after a dropped link. The cursor check above turns
			 * an already accepted first chunk into a resynchronisation. */
		s_skip = 0U;
		s_inner = total;

		/* A phone will not offer a file its own parser rejects, so a
		 * patch meant for one is dressed as an MCUboot image by
		 * `tinyfota wrap`. Step over that wrapper. A raw .wdfu has
		 * no magic here and is taken as it is, so both files work and
		 * the native transport is untouched. */
		if (len >= MCUBOOT_HDR_MIN && tinyfota_get_le32(data) == MCUBOOT_IMAGE_MAGIC) {
			uint32_t hdr_sz = tinyfota_get_le16(&data[8]);
			uint32_t img_sz = tinyfota_get_le32(&data[12]);

			if (hdr_sz < MCUBOOT_HDR_MIN || img_sz == 0U || hdr_sz > total ||
			    img_sz > total - hdr_sz) {
				e = TINYFOTA_ERR_MALFORMED;
				goto refused;
			}
			s_skip = hdr_sz;
			s_inner = img_sz;
			LOG_INF("mcuboot wrapper: %u B header, %u B payload", (unsigned)hdr_sz,
				(unsigned)img_sz);
		}

		e = begin_at(TINYFOTA_OWNER_SMP, 1U, s_inner);
		if (e != TINYFOTA_ERR_OK) {
			goto refused;
		}
		s_cli_off = 0U;
	} else if (off != s_cli_off) {
		/* Do NOT fail. mcumgr's contract is that the device reports
		 * where it actually is and the host resends from there, which
		 * is what makes a dropped chunk recoverable without restarting
		 * the whole transfer. */
		*next = s_cli_off;
		return 0;
	}

	/* The wrapper's header. */
	if (s_cli_off < s_skip) {
		size_t drop = MIN(len, (size_t)(s_skip - s_cli_off));

		data += drop;
		len -= drop;
		s_cli_off += drop;
	}

	/* The patch. */
	if (len > 0U && s_rx.got < s_inner) {
		size_t take = MIN(len, (size_t)(s_inner - s_rx.got));

		e = feed_bytes(data, take);
		if (e != TINYFOTA_ERR_OK) {
			goto refused;
		}
		len -= take;
		s_cli_off += take;
	}

	/* The wrapper's TLV trailer. Acknowledged and thrown away: the client
	 * has to be allowed to finish sending the file it holds, and nothing
	 * past the patch means anything to the bootloader. */
	s_cli_off += len;

	/* The last patch byte stages the header but does not restart the board.
	 * The host sends os_mgmt reset itself, and a board that rebooted early
	 * would drop the response and read as a failed upload. */
	if (s_rx.active && s_rx.got == s_inner) {
		e = commit_now(false);
		if (e != TINYFOTA_ERR_OK) {
			goto refused;
		}
		LOG_INF("update staged, %u B; awaiting reset", (unsigned)s_inner);
	}

	*next = s_cli_off;
	return 0;

refused:
	s_cli_off = 0U;
	LOG_WRN("upload refused at %u B (err %d)", (unsigned)s_rx.got, (int)e);
	if (s_rx.owner == TINYFOTA_OWNER_SMP) {
		rx_reset_all_locked();
	} else {
		/* A failed attempt may have parsed SMP wrapper geometry before it
		 * learned that another transport owns the receiver. Clear only SMP's
		 * cursors; the other owner's authenticated transfer remains intact. */
		smp_cursor_reset_locked();
	}
	return -EINVAL;
}

int tinyfota_rx_upload(uint32_t off, uint32_t total, const uint8_t *data, size_t len,
				uint32_t *next)
{
	int rc;

	works_bind();
	tinyfota_lock();
	rc = rx_upload_locked(off, total, data, len, next);
	tinyfota_unlock();
	return rc;
}

/**
 * Return true if a valid patch header is present in the staging flash area; otherwise return false.
 * The header's magic and ABI version must both match.
 */
bool tinyfota_rx_staged(void)
{
	struct tinyfota_hdr hdr;
	bool staged;

	works_bind();
	tinyfota_lock();
	if (staging_open() != 0) {
		tinyfota_unlock();
		return false;
	}
	if (tinyfota_flash_read(s_fa, TINYFOTA_HDR_OFFSET, &hdr, sizeof(hdr)) != 0) {
		tinyfota_unlock();
		return false;
	}
	staged = hdr.magic == TINYFOTA_MAGIC &&
		 hdr.abi_version == TINYFOTA_ABI_VERSION;
	tinyfota_unlock();
	return staged;
}

#endif /* CONFIG_TINYFOTA_SMP_IMG */
