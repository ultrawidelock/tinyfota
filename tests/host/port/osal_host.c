/*
 * osal_host.c - the host backend of the receiver's time, lock and
 * deferred-work seams in tinyfota_port.h, which IS the test double.
 *
 * A deadline-ordered list for the timers and a virtual clock the suite steps
 * (tinyfota_osal_host_advance_ms). Runs single-threaded and deterministic:
 * nothing fires until the suite says time passed.
 */
#if defined(TINYFOTA_PORT_HOST)


#include "tinyfota_host.h"

/* ---- state ---------------------------------------------------------------- */

struct host_timer {
	tinyfota_timer_fn fn;
	struct host_timer *next;
	int64_t deadline_ms;
	int pending;
};

static struct host_timer g_timers[TINYFOTA_TIMER_COUNT];
static struct host_timer *g_dwork_head; /* deadline-ordered, FIFO among equals */
static int64_t g_now_ms;

/* ---- delayable work -------------------------------------------------------- */

void tinyfota_timer_init(enum tinyfota_timer t, tinyfota_timer_fn fn)
{
	struct host_timer *dwork = &g_timers[t];

	dwork->fn = fn;
	dwork->next = NULL;
	dwork->deadline_ms = 0;
	dwork->pending = 0;
}

static void dwork_insert(struct host_timer *dwork)
{
	struct host_timer **at = &g_dwork_head;

	while (*at && (*at)->deadline_ms <= dwork->deadline_ms) {
		at = &(*at)->next;
	}
	dwork->next = *at;
	*at = dwork;
}

static void dwork_remove(struct host_timer *dwork)
{
	struct host_timer **at = &g_dwork_head;

	while (*at) {
		if (*at == dwork) {
			*at = dwork->next;
			dwork->next = NULL;
			return;
		}
		at = &(*at)->next;
	}
}

int tinyfota_timer_schedule(enum tinyfota_timer t, int32_t delay_ms)
{
	struct host_timer *dwork = &g_timers[t];

	if (dwork->pending) {
		return 0; /* the earlier deadline stands */
	}
	dwork->pending = 1;
	dwork->deadline_ms = g_now_ms + delay_ms;
	dwork_insert(dwork);
	return 0;
}

int tinyfota_timer_reschedule(enum tinyfota_timer t, int32_t delay_ms)
{
	struct host_timer *dwork = &g_timers[t];

	if (dwork->pending) {
		dwork_remove(dwork);
	}
	dwork->pending = 1;
	dwork->deadline_ms = g_now_ms + delay_ms;
	dwork_insert(dwork);
	return 0;
}

int tinyfota_timer_cancel(enum tinyfota_timer t)
{
	struct host_timer *dwork = &g_timers[t];

	if (dwork->pending) {
		dwork_remove(dwork);
		dwork->pending = 0;
	}
	return 0;
}

unsigned tinyfota_osal_host_advance_ms(int64_t ms)
{
	int64_t target = g_now_ms + ms;
	unsigned ran = 0;

	while (g_dwork_head && g_dwork_head->deadline_ms <= target) {
		struct host_timer *due = g_dwork_head;

		g_now_ms = due->deadline_ms > g_now_ms ? due->deadline_ms : g_now_ms;
		g_dwork_head = due->next;
		due->next = NULL;
		due->pending = 0; /* cleared first, so the handler may re-arm */
		due->fn();
		ran++;
	}
	g_now_ms = target;
	return ran;
}

int64_t tinyfota_osal_host_now_ms(void)
{
	return g_now_ms;
}

/* ---- reset ------------------------------------------------------------------ */

void tinyfota_osal_host_reset(void)
{
	while (g_dwork_head) {
		struct host_timer *d = g_dwork_head;

		g_dwork_head = d->next;
		d->next = NULL;
		d->pending = 0;
	}
	g_now_ms = 0;
}

/* ---- time and lock ------------------------------------------------------------ */

/* The virtual clock above. The source's host port returned real monotonic
 * time here, which nothing could step; tinyfota's claim takeover reads uptime
 * to decide whether a transfer has stalled, so the suites have to drive it. The
 * window-expiry path still does not read it on host (TINYFOTA_PORT_HOST in
 * dfu_receiver.c). */
int64_t tinyfota_uptime_ms(void)
{
	return g_now_ms;
}

/* Single-threaded: the lock cannot block, so it only counts depth. */
static int g_lock_depth;

void tinyfota_lock(void)
{
	g_lock_depth++;
}

void tinyfota_unlock(void)
{
	g_lock_depth--;
}

#endif /* TINYFOTA_PORT_HOST */
