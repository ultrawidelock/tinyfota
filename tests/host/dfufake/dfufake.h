/* dfufake — test-side control/inspection API for the DFU suites.
 *
 * The flash is the real host backend of the flash seams (tests/host/port/
 * flash_host.c): RAM partitions, erase writes 0xff, BOTH nRF alignment rules
 * enforced (word writes, page erases) -- the entire reason the applier's
 * write combiner and erase ROUND_UP exist, so a fake accepting anything would
 * hide that bug. The aliases below keep the suites' spelling. What stays fake
 * here: detools (a scripted double driving the applier's five callbacks --
 * never the patch format) and the running image the SMP group reads. PSA is a
 * knob (tests/host/psafake/psafake.h).
 */
#ifndef TINYFOTA_DFUFAKE_H
#define TINYFOTA_DFUFAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tinyfota_host.h"

/* Geometry, via the flash host backend (matches the source project's DWM3001CDK
 * partition map). */
#define DFUFAKE_STAGING_SIZE TINYFOTA_FLASH_HOST_STAGING_SIZE
#define DFUFAKE_PRIMARY_SIZE TINYFOTA_FLASH_HOST_PRIMARY_SIZE
#define DFUFAKE_WRITE_BLOCK  TINYFOTA_FLASH_HOST_WRITE_BLOCK
#define DFUFAKE_PAGE_SIZE    TINYFOTA_FLASH_HOST_PAGE_SIZE

/* The two partitions, with their knobs and recorders (see tinyfota_host.h). */
#define dfufake_staging (*tinyfota_flash_host_area(TINYFOTA_FLASH_AREA_STAGING))
#define dfufake_primary (*tinyfota_flash_host_area(TINYFOTA_FLASH_AREA_PRIMARY))

/**
 * Everything the suites drive or inspect that is not flash: the scripted
 * detools double. (The reboot recorder is tinyfota_flash_host_reboots().)
 */
struct dfufake_state {
	/* detools double: statuses returned by each entry point. */
	int detools_init_ret;
	int detools_process_ret;
	int detools_finalize_ret;
	unsigned detools_init_calls, detools_process_calls, detools_finalize_calls;
	size_t detools_patch_size; /* patch_size argument seen at init */
	size_t detools_fed;        /* total patch bytes handed to _process */
};

extern struct dfufake_state dfufake;

/**
 * The bytes dfu_smp_img.c reads as the RUNNING image. On target that is the
 * primary slot mapped at PM_MCUBOOT_PAD_ADDRESS and read straight through a
 * pointer; here pm_config.h points that macro at this buffer, so a suite lays
 * out a header and a TLV block and then asks what the module reports.
 */
extern uint8_t dfufake_running_image[];

/** @brief Zero every recording, restore every knob, erase both partitions,
 * drop queued OSAL work. */
void dfufake_reset(void);

/** @brief Fill @p area with 0xff without going through tinyfota_flash_erase(). */
void dfufake_blank(struct tinyfota_flash_host_area *area);

/** @brief Byte at @p off of @p area, for assertions about what landed. */
uint8_t dfufake_peek(const struct tinyfota_flash_host_area *area, size_t off);

/* ---- the scripted detools double ------------------------------------------
 *
 * A suite builds a list of these and detools_apply_patch_in_place_process()
 * replays it against the applier's callbacks, in order, on its FIRST call.
 * Anything the callbacks reject stops the replay and _process() returns
 * DFUFAKE_DETOOLS_CALLBACK_FAILED, which is how the applier's error paths are
 * reached without inventing a patch format.
 */
enum dfufake_op_kind {
	DFUFAKE_OP_WRITE,  /**< mem_write(dst, bytes, len) */
	DFUFAKE_OP_READ,   /**< mem_read(src, len) — forces a full flush first */
	DFUFAKE_OP_ERASE,  /**< mem_erase(addr, len) */
	DFUFAKE_OP_STEP_SET, /**< step_set(step) */
	DFUFAKE_OP_STEP_GET, /**< step_get() — result recorded below */
};

/** One scripted memory operation handed to the applier's callbacks. */
struct dfufake_op {
	enum dfufake_op_kind kind;
	uintptr_t addr;
	size_t len;
	int step;
	uint8_t fill; /**< byte written by DFUFAKE_OP_WRITE */
};

#define DFUFAKE_MAX_OPS 64

/** @brief Replace the script with @p count operations from @p ops. */
void dfufake_script(const struct dfufake_op *ops, size_t count);

/** @brief Result of the last DFUFAKE_OP_STEP_GET the script replayed. */
int dfufake_last_step_get(void);

/** @brief How many scripted operations the callbacks accepted before stopping. */
size_t dfufake_ops_done(void);

/** Returned by the double when an applier callback refused an operation. */
#define DFUFAKE_DETOOLS_CALLBACK_FAILED (-6001)

#endif /* TINYFOTA_DFUFAKE_H */
