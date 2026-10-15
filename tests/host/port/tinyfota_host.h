/* SPDX-License-Identifier: ISC */

/*
 * tinyfota_host.h - suite controls for the host port (flash_host.c,
 * osal_host.c). Test-only: nothing under core/ or ports/ includes it.
 */
#ifndef TINYFOTA_HOST_H
#define TINYFOTA_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "tinyfota/tinyfota_port.h"

/* ---- flash (flash_host.c) -------------------------------------------------
 *
 * The values the source suite was written against. The primary slot matches
 * the partition map (samples/nrf52833/pm_static.yml, 0x6a000). Staging does
 * not: the map's patch_staging is 0x8000, and these 0xa000 bytes are the size
 * it had before the source project shrank it (its commit 50e762d5). Kept, because the imported
 * suite's size-limit cases are written against it.
 */
#define TINYFOTA_FLASH_HOST_WRITE_BLOCK  4u
#define TINYFOTA_FLASH_HOST_PAGE_SIZE    4096u
#define TINYFOTA_FLASH_HOST_STAGING_SIZE 0xa000u  /* 40,960 B */
#define TINYFOTA_FLASH_HOST_PRIMARY_SIZE 0x6a000u /* 434,176 B */

/**
 * One RAM-backed area plus the failure knobs a suite injects. Every
 * `*_fail_in` counts calls down: -1 never fails, 0 fails this call and every
 * call after it, N > 0 lets N calls through and then fails.
 */
struct tinyfota_flash_host_area {
	uint8_t *buf;
	size_t size;

	/* knobs */
	int fail_open;
	int write_fail_in;
	int erase_fail_in;
	int read_fail_in;

	/* recorded */
	unsigned open_calls, close_calls, write_calls, erase_calls, read_calls;
	uint32_t last_write_off;
	size_t last_write_len;
	uint32_t last_erase_off;
	size_t last_erase_len;
};

/** @brief The area behind @p id, for knobs and byte-level assertions. */
struct tinyfota_flash_host_area *tinyfota_flash_host_area(enum tinyfota_flash_area_id id);

/** @brief tinyfota_reboot() calls recorded since reset. */
unsigned tinyfota_flash_host_reboots(void);

/** @brief Zero every recording, restore every knob, erase both areas. */
void tinyfota_flash_host_reset(void);

/* ---- deferred work and time (osal_host.c) ---------------------------------
 *
 * The virtual clock starts at 0 and moves only when a suite advances it, and
 * tinyfota_uptime_ms reads it, so timers and uptime never disagree.
 */

/** @brief Disarm every timer, clock back to 0. */
void tinyfota_osal_host_reset(void);

/** @brief Advance the virtual clock, running due timers in deadline order.
 * @return how many handlers ran. */
unsigned tinyfota_osal_host_advance_ms(int64_t ms);

/** @brief The virtual clock. */
int64_t tinyfota_osal_host_now_ms(void);

#endif /* TINYFOTA_HOST_H */
