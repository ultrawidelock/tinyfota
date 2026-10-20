/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/* tbfake: logfake's <zephyr/kernel.h> plus the delayable work dfu_tinyble.c
 * schedules. The suite runs it on a virtual clock of its own
 * (tbfake_advance_ms()), single-threaded, as the system work queue would:
 * one item at a time, never from inside the call that schedules it. */
#ifndef TBFAKE_ZEPHYR_KERNEL_H
#define TBFAKE_ZEPHYR_KERNEL_H

#include "../../logfake/zephyr/kernel.h"

struct k_work {
	int unused;
};

typedef void (*k_work_handler_t)(struct k_work *work);

struct k_work_delayable {
	struct k_work work;
	k_work_handler_t handler;
	int64_t due_ms;
	bool pending;
};

typedef struct {
	int64_t ms;
} k_timeout_t;

#define K_MSEC(ms) ((k_timeout_t){(ms)})
#define K_NO_WAIT  K_MSEC(0)

#define K_WORK_DELAYABLE_DEFINE(name, fn) struct k_work_delayable name = {.handler = (fn)}

/** Earlier deadline stands while pending, as on Zephyr. */
int k_work_schedule(struct k_work_delayable *dwork, k_timeout_t delay);
/** Restarts the delay. */
int k_work_reschedule(struct k_work_delayable *dwork, k_timeout_t delay);
int k_work_cancel_delayable(struct k_work_delayable *dwork);

#endif /* TBFAKE_ZEPHYR_KERNEL_H */
