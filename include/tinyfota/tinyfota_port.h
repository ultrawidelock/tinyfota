/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Everything the tinyfota core needs from its platform, in one header.
 *
 * A port implements the functions declared here and nothing else. `make seams`
 * derives the list from the compiled core objects (undefined minus defined
 * globals) and checks it against tests/seams.expected, so a seam cannot be
 * added or dropped without that check failing. PORTING.md describes each one.
 *
 * Who needs what. The applier, which runs inside the bootloader, needs only
 * the flash seams. The receiver, which runs in the application, needs all of
 * them. Ports: ports/zephyr/ (target), tests/host/ (the host fakes).
 *
 * Flash geometry is compile-time, not a seam: the erase page is
 * TINYFOTA_PAGE_SIZE (4096 B, part of the staging format in tinyfota.h) and the
 * write block is 4 B. A device whose erase page does not divide 4096 B, or
 * whose write block does not divide 4 B, cannot host this format. The Zephyr
 * port checks both when it opens an area and refuses otherwise.
 */

#ifndef TINYFOTA_PORT_H_
#define TINYFOTA_PORT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- byte order (header-only, not a seam) ------------------------------- */

static inline uint16_t tinyfota_get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t tinyfota_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static inline void tinyfota_put_le32(uint32_t v, uint8_t *p)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* ---- flash --------------------------------------------------------------- */

enum tinyfota_flash_area_id {
	TINYFOTA_FLASH_AREA_PRIMARY, /* the running image's slot */
	TINYFOTA_FLASH_AREA_STAGING, /* where a delta patch lands */
};

struct tinyfota_flash_area; /* opaque; each port owns the layout */

/** @brief Open @p id. @return 0 and *@p fa set, negative on failure. */
int tinyfota_flash_open(enum tinyfota_flash_area_id id, const struct tinyfota_flash_area **fa);
void tinyfota_flash_close(const struct tinyfota_flash_area *fa);

/** @brief Size of the area in bytes. */
size_t tinyfota_flash_size(const struct tinyfota_flash_area *fa);

/** @return 0, negative on out-of-bounds or a backend failure. */
int tinyfota_flash_read(const struct tinyfota_flash_area *fa, uint32_t off, void *dst, size_t len);
/** @brief Write; @p off and @p len must be 4-byte aligned. */
int tinyfota_flash_write(const struct tinyfota_flash_area *fa, uint32_t off, const void *src,
			 size_t len);
/** @brief Erase to 0xff; @p off and @p len must be page aligned. */
int tinyfota_flash_erase(const struct tinyfota_flash_area *fa, uint32_t off, size_t len);

/* ---- reboot, time, lock (receiver only) ---------------------------------- */

/** @brief Cold reboot into the bootloader, which applies a staged patch. */
void tinyfota_reboot(void);

/** @brief Monotonic milliseconds since boot. Times the update window. */
int64_t tinyfota_uptime_ms(void);

/**
 * @brief The receiver's one lock. Every transport calls into the receiver from
 * its own context (BLE RX, a work queue, mcumgr), and this serialises them. A
 * port with a single thread of control may make both functions no-ops.
 */
void tinyfota_lock(void);
void tinyfota_unlock(void);

/* ---- deferred work (receiver only) --------------------------------------- */

/** The receiver's two deferred jobs. The port owns their storage. */
enum tinyfota_timer {
	TINYFOTA_TIMER_REBOOT, /* reboot after the COMMIT reply has gone out */
	TINYFOTA_TIMER_WINDOW, /* close the update window */
	TINYFOTA_TIMER_COUNT,
};

typedef void (*tinyfota_timer_fn)(void);

/** @brief Bind @p fn to @p t. Called once, before any of the three below. */
void tinyfota_timer_init(enum tinyfota_timer t, tinyfota_timer_fn fn);
/** @brief Run @p t after @p delay_ms; a NO-OP while it is armed (the earlier
 * deadline stands). Must not run @p fn from inside this call. */
int tinyfota_timer_schedule(enum tinyfota_timer t, int32_t delay_ms);
/** @brief Run @p t after @p delay_ms, RESTARTING the delay if it was armed. */
int tinyfota_timer_reschedule(enum tinyfota_timer t, int32_t delay_ms);
/** @brief Disarm @p t if armed; an idle one is left alone. */
int tinyfota_timer_cancel(enum tinyfota_timer t);

/* ---- signature (receiver only) ------------------------------------------- */

/**
 * @brief ECDSA P-256 over SHA-256 of @p msg.
 * @param pub X9.62 uncompressed point, 0x04 || X || Y.
 * @param sig raw r || s.
 * @return 0 only when the signature verifies. Any provider init the port needs
 *         happens in here, idempotently, and a failed init returns non-zero.
 */
int tinyfota_ecdsa_p256_verify(const uint8_t pub[65], const uint8_t *msg, size_t msg_len,
			       const uint8_t sig[64]);

/* ---- logging (optional) and the boot hook --------------------------------- */

#if defined(__ZEPHYR__)

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#define TINYFOTA_PRINTK(...) printk(__VA_ARGS__)
/* APPLICATION level: after the flash driver (POST_KERNEL), before MCUboot's
 * main(). The priority is load-bearing for the applier. */
#define TINYFOTA_INIT_APPLICATION_PRIO(fn, prio) SYS_INIT(fn, APPLICATION, prio)

#else /* !__ZEPHYR__ */

/* Each guarded, so a platform logger included first keeps its own. */
#ifndef LOG_MODULE_REGISTER
/* Expands to a declaration, so the `;` after it is not a stray one. */
#define LOG_MODULE_REGISTER(...) struct tinyfota_log_module_unused
#endif
#ifndef LOG_MODULE_DECLARE
#define LOG_MODULE_DECLARE(...) struct tinyfota_log_module_unused
#endif

#if defined(TINYFOTA_PORT_LOG) && TINYFOTA_PORT_LOG
/* Optional seam: a port that wants the core's diagnostics defines
 * TINYFOTA_PORT_LOG=1 and provides this. @p level is 1 (error) to 4 (debug). */
void tinyfota_log(int level, const char *fmt, ...);
#define TINYFOTA_LOG_FN(level, ...) tinyfota_log((level), __VA_ARGS__)
#else
#define TINYFOTA_LOG_FN(level, ...) ((void)0)
#endif
#ifndef LOG_ERR
#define LOG_ERR(...) TINYFOTA_LOG_FN(1, __VA_ARGS__)
#define LOG_WRN(...) TINYFOTA_LOG_FN(2, __VA_ARGS__)
#define LOG_INF(...) TINYFOTA_LOG_FN(3, __VA_ARGS__)
#define LOG_DBG(...) TINYFOTA_LOG_FN(4, __VA_ARGS__)
#endif
#define TINYFOTA_PRINTK(...) TINYFOTA_LOG_FN(3, __VA_ARGS__)

/* Without SYS_INIT, the applier's entry point is exported as a pointer named
 * tinyfota_init_<fn>. A bootloader calls it before it validates the primary
 * slot; the host tests call it to run one boot. */
#define TINYFOTA_INIT_APPLICATION_PRIO(fn, prio) int (*const tinyfota_init_##fn)(void) = (fn)

#endif /* __ZEPHYR__ */

#ifdef __cplusplus
}
#endif

#endif /* TINYFOTA_PORT_H_ */
