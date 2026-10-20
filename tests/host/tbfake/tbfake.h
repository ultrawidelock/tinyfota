/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/* tbfake: suite controls for the tinyble and delayable-work doubles that
 * ports/zephyr/dfu_tinyble.c builds against (tests/host/test_tinyble.c).
 *
 * tb_notify() records every notification, refuses one longer than MTU - 3 as
 * tinyble does, and returns whatever the knobs say: TB_EBUSY for the next
 * busy_n calls, or a fixed error. The lock is counted, so a notification sent
 * without it is recorded as a violation. Work runs only when the suite moves
 * the clock. */
#ifndef TBFAKE_H
#define TBFAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TBFAKE_MAX_NOTIFY 64
#define TBFAKE_MAX_BYTES  1024

struct tbfake_notify {
	uint16_t handle;
	uint16_t len;
	size_t at; /**< offset of its bytes in tbfake.bytes */
};

struct tbfake_state {
	/* knobs */
	uint16_t mtu;
	int busy_n;   /**< return TB_EBUSY for this many more calls */
	int fail_ret; /**< non-zero: every call returns this */

	/* recorded */
	struct tbfake_notify notify[TBFAKE_MAX_NOTIFY];
	size_t notify_count;
	uint8_t bytes[TBFAKE_MAX_BYTES];
	size_t bytes_len;
	unsigned calls;           /**< tb_notify() calls, refused ones included */
	unsigned unlocked_calls;  /**< tb_notify() calls made without the lock */
	unsigned oversize_calls;  /**< tb_notify() calls longer than MTU - 3 */
	int lock_depth;
};

extern struct tbfake_state tbfake;

/** @brief Zero every recording, MTU back to 23, drop every scheduled item. */
void tbfake_reset(void);

/** @brief Move the work clock on, running every item that falls due. */
unsigned tbfake_advance_ms(int64_t ms);

/** @brief The bytes notified on @p handle, concatenated, into @p out. */
size_t tbfake_stream(uint16_t handle, uint8_t *out, size_t cap);

/** @brief Forget the recorded notifications only. */
void tbfake_clear_notify(void);

#endif /* TBFAKE_H */
