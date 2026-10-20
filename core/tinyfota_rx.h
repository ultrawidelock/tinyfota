/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Receives a delta patch into the staging partition, application side.
 *
 * Transport-independent on purpose. The Zephyr port feeds this from its own
 * L2CAP CoC, a GATT characteristic and SMP, but nothing here knows that -- it takes
 * frames and returns replies, so the host tests can drive it without a radio.
 *
 * The bootloader half is @ref tinyfota.h. This side never applies anything: it
 * writes bytes, checks a signature, and reboots.
 */

#ifndef TINYFOTA_RX_H_
#define TINYFOTA_RX_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Version-2 request opcodes, first byte of every frame from the host.
 *
 * These deliberately do not overlap the original transfer-blind 0x01..0x04
 * protocol. Old hosts and firmware therefore fail loudly instead of
 * misinterpreting a transfer id as a length.
 */
enum tinyfota_op {
	TINYFOTA_OP_BEGIN = 0x11,  /**< u32 transfer id + u32 total follow */
	TINYFOTA_OP_DATA = 0x12,   /**< u32 transfer id + u32 offset + bytes */
	TINYFOTA_OP_COMMIT = 0x13, /**< u32 transfer id; reboots on success */
	TINYFOTA_OP_ABORT = 0x14,  /**< u32 transfer id; erases staged data */
};

/** Reply opcodes, first byte of every frame back to the host. */
enum tinyfota_rsp {
	TINYFOTA_RSP_OK = 0x81,  /**< u32 transfer id + u32 next offset follow */
	TINYFOTA_RSP_ERR = 0x82, /**< one @ref tinyfota_err byte follows */
};

/**
 * Why a frame was refused.
 *
 * Deliberately coarse. A peer that has not been let in learns only that it was
 * refused, not how close it got.
 */
enum tinyfota_err {
	TINYFOTA_ERR_CLOSED = 1,    /**< no update window is open */
	TINYFOTA_ERR_SEQUENCE = 2,  /**< opcode does not fit the current state */
	TINYFOTA_ERR_SIZE = 3,      /**< will not fit patch_staging */
	TINYFOTA_ERR_AUTH = 4,      /**< header signature did not verify */
	TINYFOTA_ERR_INTEGRITY = 5, /**< length or CRC disagreed at commit */
	TINYFOTA_ERR_FLASH = 6,     /**< a write or erase failed */
	TINYFOTA_ERR_MALFORMED = 7, /**< frame too short for its opcode */
	TINYFOTA_ERR_BUSY = 8,      /**< another transport owns the receiver */
	TINYFOTA_ERR_BASE = 9,      /**< signed for a different image than the one running */
};

/** Largest reply this ever produces. */
#define TINYFOTA_RSP_MAX 9u

/**
 * How long a stalled transfer holds the receiver before another peer's BEGIN
 * may take it over. Kconfig sets it on Zephyr, where zephyr/Kconfig says what
 * it bounds; every other port and the host suites get the same default.
 */
#ifndef CONFIG_TINYFOTA_CLAIM_IDLE_MS
#define CONFIG_TINYFOTA_CLAIM_IDLE_MS 10000
#endif

/**
 * A receiver owner. Each transport gets a distinct value so a disconnect or
 * malformed request on one endpoint cannot discard another endpoint's upload.
 */
enum tinyfota_owner {
	TINYFOTA_OWNER_NONE = 0,
	TINYFOTA_OWNER_L2CAP = 1,
	TINYFOTA_OWNER_GATT = 2,
	TINYFOTA_OWNER_SMP = 3,
	TINYFOTA_OWNER_TEST = 4,
};

/**
 * Open the update window for @p duration_ms.
 *
 * Until this is called nothing is accepted, and that IS the authorization
 * model. The patch is signed and MCUboot re-verifies the result, so no peer can
 * install code regardless; what the window prevents is an unauthenticated
 * peer in radio range burning flash cycles and forcing reboots. A device
 * that anyone nearby can reset in a loop is a real availability attack, and a
 * window the owner has to open is what stops it.
 *
 * Calling it again while open restarts the clock.
 */
void tinyfota_window_open(uint32_t duration_ms);

/** Close the window immediately and discard any transfer in progress. */
void tinyfota_window_close(void);

/** True while the window is open. Transports gate their accept() on this. */
bool tinyfota_window_is_open(void);

/**
 * Called whenever the window opens or closes.
 *
 * Registered rather than a weak symbol so that it survives LTO without
 * argument, and so a port with no indicator pays nothing.
 */
typedef void (*tinyfota_window_cb)(bool open);

/**
 * Watch the window, so the board can SHOW that it is open.
 *
 * There can be several ways in -- a button, an app command, a bench SWD
 * write -- and none of them is visible from outside the board. An owner who
 * pressed the button has no way to tell whether the press registered, and the
 * five-minute window can expire while they are still looking for the phone.
 * One callback covers every path because they all end at
 * tinyfota_window_open().
 */
void tinyfota_set_window_cb(tinyfota_window_cb cb);

/**
 * Handle one frame.
 *
 * @param frame     request bytes, opcode first
 * @param len       length of @p frame
 * @param rsp       at least @ref TINYFOTA_RSP_MAX bytes
 * @param rsp_len   set to the number of reply bytes produced
 *
 * @retval 0 always; failures are reported to the peer through @p rsp, because
 *           a transport has nothing useful to do with an error code.
 */
int tinyfota_rx_frame(enum tinyfota_owner owner, const uint8_t *frame, size_t len,
			      uint8_t *rsp, size_t *rsp_len);

struct tinyfota_hdr; /* tinyfota.h */

/**
 * Called at COMMIT, once the staged bytes have proven themselves.
 *
 * WHY THIS EXISTS. On the DWM3001CDK, COMMIT's job is finished when the header
 * lands: MCUboot reads it on the next boot and applies the patch, so the
 * receiver's whole contract with the bootloader is those 32 bytes on flash.
 *
 * The ESP32 has no such bootloader. Its two OTA slots are selected by an
 * otadata partition that only the application can write, so a staged image
 * that nobody points the bootloader at is simply ignored -- the board reboots
 * into what it was already running and the update silently does not happen.
 * Something has to run between "the bytes are good" and "reboot", and this is
 * it.
 *
 * Called AFTER the magic, ABI, header CRC, length and patch CRC have all
 * passed and after the header write, and BEFORE the reboot is armed. Returning
 * non-zero fails the COMMIT with @ref TINYFOTA_ERR_FLASH and leaves
 * nothing staged, so a port that cannot arm its bootloader refuses the update
 * rather than rebooting into the old image and reporting success.
 *
 * @param hdr the verified header, including @ref tinyfota_hdr.flags --
 *            which is how a port tells a whole-image payload from a delta.
 * @return 0 to accept.
 */
typedef int (*tinyfota_commit_cb)(const struct tinyfota_hdr *hdr);

/**
 * Install the commit hook. NULL (the default) means "nothing to do", which is
 * every port with a bootloader that reads staging by itself.
 */
void tinyfota_set_commit_cb(tinyfota_commit_cb cb);

/** Drop the transfer only when @p owner currently owns it. */
void tinyfota_rx_reset(enum tinyfota_owner owner);

/** Drop all receiver state. Window close and tests use this global boundary. */
void tinyfota_rx_reset_all(void);

/** Erase staging only if it is unowned or owned by @p owner. */
int tinyfota_rx_erase(enum tinyfota_owner owner);

#ifdef CONFIG_TINYFOTA_BLE
/**
 * Start the Zephyr port's own triggers (ports/zephyr/dfu_ble_zephyr.c):
 * register the update PSM (CONFIG_TINYFOTA_BLE_COC) and arm the sw0 button
 * (CONFIG_TINYFOTA_BUTTON). Call once, after bt_enable().
 *
 * @retval 0 on success, or with no usable button (software-only window)
 * @retval negative when the L2CAP server cannot be registered
 */
int tinyfota_ble_start(void);
#endif

#ifdef CONFIG_TINYFOTA_SMP_IMG
/**
 * Take one SMP image-upload chunk.
 *
 * The same bytes and the same checks as @ref tinyfota_rx_frame, reached from
 * CBOR instead of opcodes, so that a stock mcumgr client (nRF Device Manager,
 * `mcumgr image upload`) can push an update. See src/dfu_smp_img.c.
 *
 * @param off    offset the host believes this chunk starts at
 * @param total  whole wire length; only read when @p off is 0
 * @param data   chunk bytes
 * @param len    length of @p data
 * @param[out] next  offset to send next. On a mismatched @p off this comes back
 *                   as the device's real position and the chunk is discarded --
 *                   a resync, which the protocol treats as success.
 *
 * @retval 0        chunk accepted, or a resync was requested
 * @retval -EACCES  no update window is open
 * @retval -EBUSY   another transport owns the receiver
 * @retval -EINVAL  refused; the transfer is discarded and must restart at 0
 */
int tinyfota_rx_upload(uint32_t off, uint32_t total, const uint8_t *data, size_t len,
		      uint32_t *next);

/** True when a complete, verified update is staged and waiting for a reboot. */
bool tinyfota_rx_staged(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TINYFOTA_RX_H_ */
