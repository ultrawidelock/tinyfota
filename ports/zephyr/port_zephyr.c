/* SPDX-License-Identifier: Apache-2.0 */

/*
 * port_zephyr.c - the Zephyr backend of the receiver's time, lock and
 * deferred-work seams in tinyfota_port.h: 1:1 onto k_uptime_get, k_mutex and
 * k_work_delayable on the system workqueue, plus the trampoline the k_work
 * handler signature needs. Semantics are Zephyr's own: schedule is a no-op
 * while pending (the earlier deadline stands), reschedule restarts the delay.
 */
#if defined(__ZEPHYR__)

#include <zephyr/kernel.h>

#include "tinyfota/tinyfota_port.h"

static K_MUTEX_DEFINE(s_lock);

static struct {
	struct k_work_delayable kw;
	tinyfota_timer_fn fn;
} s_timers[TINYFOTA_TIMER_COUNT];

int64_t tinyfota_uptime_ms(void)
{
	return k_uptime_get();
}

void tinyfota_lock(void)
{
	(void)k_mutex_lock(&s_lock, K_FOREVER);
}

void tinyfota_unlock(void)
{
	(void)k_mutex_unlock(&s_lock);
}

static void timer_tramp(struct k_work *kw)
{
	struct k_work_delayable *dk = k_work_delayable_from_work(kw);

	for (size_t i = 0; i < ARRAY_SIZE(s_timers); i++) {
		if (&s_timers[i].kw == dk && s_timers[i].fn != NULL) {
			s_timers[i].fn();
			return;
		}
	}
}

void tinyfota_timer_init(enum tinyfota_timer t, tinyfota_timer_fn fn)
{
	s_timers[t].fn = fn;
	k_work_init_delayable(&s_timers[t].kw, timer_tramp);
}

int tinyfota_timer_schedule(enum tinyfota_timer t, int32_t delay_ms)
{
	int rc = k_work_schedule(&s_timers[t].kw, K_MSEC(delay_ms));

	return rc >= 0 ? 0 : rc;
}

int tinyfota_timer_reschedule(enum tinyfota_timer t, int32_t delay_ms)
{
	int rc = k_work_reschedule(&s_timers[t].kw, K_MSEC(delay_ms));

	return rc >= 0 ? 0 : rc;
}

int tinyfota_timer_cancel(enum tinyfota_timer t)
{
	k_work_cancel_delayable(&s_timers[t].kw);
	return 0;
}

#endif /* __ZEPHYR__ */
