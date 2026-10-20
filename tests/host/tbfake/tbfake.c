/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/* tbfake: recording tb_notify(), a counted tb_zephyr_lock(), and delayable work
 * on a virtual clock. See tbfake.h. */

#include "tbfake.h"

#include <string.h>

#include <tinyble/tb_zephyr.h>
#include <tinyble/tinyble.h>
#include <zephyr/kernel.h>

struct tbfake_state tbfake;

#define TBFAKE_MAX_WORK 8

static struct k_work_delayable *s_work[TBFAKE_MAX_WORK];
static size_t s_work_count;
static int64_t s_now_ms;

void tbfake_clear_notify(void)
{
	tbfake.notify_count = 0;
	tbfake.bytes_len = 0;
}

void tbfake_reset(void)
{
	memset(&tbfake, 0, sizeof(tbfake));
	tbfake.mtu = 23;
	for (size_t i = 0; i < s_work_count; i++) {
		s_work[i]->pending = false;
	}
	s_now_ms = 0;
}

size_t tbfake_stream(uint16_t handle, uint8_t *out, size_t cap)
{
	size_t n = 0;

	for (size_t i = 0; i < tbfake.notify_count; i++) {
		const struct tbfake_notify *nt = &tbfake.notify[i];

		if (nt->handle != handle || n + nt->len > cap) {
			continue;
		}
		memcpy(out + n, tbfake.bytes + nt->at, nt->len);
		n += nt->len;
	}
	return n;
}

/* ---- tinyble ---------------------------------------------------------------- */

int tb_notify(uint16_t handle, const uint8_t *data, uint16_t len)
{
	struct tbfake_notify *nt;

	tbfake.calls++;
	if (tbfake.lock_depth <= 0) {
		tbfake.unlocked_calls++;
	}
	if (tbfake.fail_ret != 0) {
		return tbfake.fail_ret;
	}
	if (tbfake.busy_n > 0) {
		tbfake.busy_n--;
		return TB_EBUSY;
	}
	if (len > tbfake.mtu - 3u) {
		tbfake.oversize_calls++;
		return TB_EINVAL;
	}
	if (tbfake.notify_count >= TBFAKE_MAX_NOTIFY || tbfake.bytes_len + len > TBFAKE_MAX_BYTES) {
		return TB_EINVAL;
	}
	nt = &tbfake.notify[tbfake.notify_count++];
	nt->handle = handle;
	nt->len = len;
	nt->at = tbfake.bytes_len;
	memcpy(tbfake.bytes + tbfake.bytes_len, data, len);
	tbfake.bytes_len += len;
	return 0;
}

uint16_t tb_mtu(void)
{
	return tbfake.mtu;
}

void tb_zephyr_lock(void)
{
	tbfake.lock_depth++;
}

void tb_zephyr_unlock(void)
{
	tbfake.lock_depth--;
}

/* ---- delayable work ----------------------------------------------------------- */

static void track(struct k_work_delayable *dwork)
{
	for (size_t i = 0; i < s_work_count; i++) {
		if (s_work[i] == dwork) {
			return;
		}
	}
	if (s_work_count < TBFAKE_MAX_WORK) {
		s_work[s_work_count++] = dwork;
	}
}

int k_work_schedule(struct k_work_delayable *dwork, k_timeout_t delay)
{
	track(dwork);
	if (dwork->pending) {
		return 0;
	}
	dwork->pending = true;
	dwork->due_ms = s_now_ms + delay.ms;
	return 1;
}

int k_work_reschedule(struct k_work_delayable *dwork, k_timeout_t delay)
{
	track(dwork);
	dwork->pending = true;
	dwork->due_ms = s_now_ms + delay.ms;
	return 1;
}

int k_work_cancel_delayable(struct k_work_delayable *dwork)
{
	dwork->pending = false;
	return 0;
}

unsigned tbfake_advance_ms(int64_t ms)
{
	const int64_t target = s_now_ms + ms;
	unsigned ran = 0;

	for (;;) {
		struct k_work_delayable *due = NULL;

		for (size_t i = 0; i < s_work_count; i++) {
			if (s_work[i]->pending && s_work[i]->due_ms <= target &&
			    (due == NULL || s_work[i]->due_ms < due->due_ms)) {
				due = s_work[i];
			}
		}
		if (due == NULL) {
			break;
		}
		if (due->due_ms > s_now_ms) {
			s_now_ms = due->due_ms;
		}
		due->pending = false; /* cleared first, so the handler may re-arm */
		due->handler(&due->work);
		ran++;
	}
	s_now_ms = target;
	return ran;
}
