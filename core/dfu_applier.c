/* SPDX-License-Identifier: ISC */

/**
 * @file
 * @brief Applies a staged delta patch onto the primary slot, from inside
 *        MCUboot.
 *
 * SYS_INIT at APPLICATION level: after the flash driver (POST_KERNEL), before
 * MCUboot's main() -- the only window in which the primary slot can be
 * rewritten, and no upstream MCUboot line is edited. It cannot live in the
 * application, which executes from the slot being rewritten. A normal boot
 * costs one word read (header magic mismatch, return).
 *
 * SAFETY: the header CRCs itself (torn write fails); patch and from-image are
 * CRC-checked before a byte is erased; and MCUboot re-verifies the P-256
 * signature of the RESULT before booting it, dropping to serial recovery on
 * failure. The worst a forged patch achieves is destroying the installed image
 * (recoverable), not installing code. Power cuts are expected -- this rewrites
 * most of 442 KB (ultrawidelock's slot) over seconds; detools' step counter (step_set/step_get) is
 * what makes that survivable.
 */

#include <string.h>

#include "dfu_crc.h"
#include "tinyfota.h"
#include "tinyfota/tinyfota_port.h"
#include "detools.h"

#if defined(CONFIG_TINYFOTA_APPLIER_LOG)
#define DFU_LOG(...) TINYFOTA_PRINTK("WDFU " __VA_ARGS__) /* printk on every platform */
#else
#define DFU_LOG(...) ((void)0)
#endif

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef ROUND_UP
#define ROUND_UP(x, align) ((((x) + (align) - 1) / (align)) * (align))
#endif

/**
 * The region the patch transforms: MCUboot's primary slot, header and all.
 *
 * Not mcuboot_primary_app. The patch is generated between two SIGNED images,
 * because the signature and its TLVs are part of what has to change -- patching
 * only the payload would leave the old signature in front of new code and
 * MCUboot would refuse to boot it.
 */
/** Most steps the one-page step log can record, one word each. */
#define STEP_MAX (TINYFOTA_PAGE_SIZE / sizeof(uint32_t))

/* The host builds this header with struct.pack and this side reads it with a
 * flash read straight into the struct, so the two agree on the layout or
 * nothing works. Caught here rather than on the bench. */
_Static_assert(sizeof(struct tinyfota_hdr) == TINYFOTA_HDR_LEN,
	       "tinyfota_hdr changed size; tools/tinyfota must change with it");
_Static_assert(TINYFOTA_HDR_CRC_LEN == TINYFOTA_HDR_LEN - sizeof(uint32_t),
	       "the header CRC must cover everything except itself");

/**
 * Context passed to detools patch applier callbacks holding the primary and staging flash areas.
 */
struct apply_ctx {
	const struct tinyfota_flash_area *primary;
	const struct tinyfota_flash_area *staging;
};

/* Static, not automatic: this runs on the main thread's stack and the patcher
 * state is a few hundred bytes. MCUboot's stack has an ECDSA verify to do
 * afterwards. */
static struct apply_ctx s_ctx;
static struct detools_apply_patch_in_place_t s_patcher;
static uint8_t s_chunk[CONFIG_TINYFOTA_APPLIER_CHUNK];

/* ---- detools memory callbacks -------------------------------------------- */
/*
 * Addresses are offsets within the primary slot, 0-based, which is exactly what
 * flash_area_* takes.
 *
 * WRITES ARE COMBINED, and that is not an optimisation. detools writes whatever
 * its decompressor produced -- `to_size = MIN(sizeof(to), chunk_size)` at
 * detools.c:1936 -- so both the address and the length are arbitrary. nRF flash
 * has a 4-byte write block, so handing those straight to flash_area_write()
 * fails on any chunk that is not word-aligned and word-sized, which is most of
 * them. The shift loop earlier in the patch (detools.c:1690-1711) happens to
 * use 128-byte aligned chunks and works either way; the patch phase does not.
 *
 * So bytes accumulate here and only whole words go to flash. A partial tail is
 * held until either the next contiguous write completes the word, or something
 * proves the region is finished -- a write somewhere else, a read, an erase, or
 * the end of the patch -- at which point it is padded with 0xff. Padding is
 * correct there precisely because the region IS finished: detools has moved on,
 * and the surrounding flash was left erased.
 */

/** Word-aligned staging for the write combiner. 128 matches detools' own. */
#define WBUF_SZ 128

static struct {
	uint32_t addr; /**< primary-slot offset that buf[0] belongs at */
	size_t len;    /**< bytes currently held */
	uint8_t buf[WBUF_SZ];
} s_w;

/** Push out every complete word we are holding. */
static int wbuf_flush_words(struct apply_ctx *c)
{
	size_t whole = s_w.len & ~(size_t)3;

	if (whole == 0U) {
		return 0;
	}
	if (tinyfota_flash_write(c->primary, s_w.addr, s_w.buf, whole) != 0) {
		DFU_LOG("w %u+%u failed\n", (unsigned)s_w.addr, (unsigned)whole);
		return -1;
	}

	s_w.len -= whole;
	s_w.addr += (uint32_t)whole;
	if (s_w.len > 0U) {
		memmove(s_w.buf, s_w.buf + whole, s_w.len);
	}
	return 0;
}

/** Push out everything, padding a partial tail to a word with erased bits. */
static int wbuf_flush_all(struct apply_ctx *c)
{
	if (wbuf_flush_words(c) != 0) {
		return -1;
	}
	if (s_w.len == 0U) {
		return 0;
	}

	while (s_w.len & 3U) {
		s_w.buf[s_w.len++] = 0xff;
	}
	if (tinyfota_flash_write(c->primary, s_w.addr, s_w.buf, s_w.len) != 0) {
		DFU_LOG("wt %u+%u failed\n", (unsigned)s_w.addr, (unsigned)s_w.len);
		return -1;
	}

	s_w.addr += (uint32_t)s_w.len;
	s_w.len = 0U;
	return 0;
}

/**
 * Write bytes to the primary flash area during patch application by buffering them and flushing
 * when the buffer reaches word size or on a discontiguous write; returns -1 on failure.
 */
static int mem_write(void *arg_p, uintptr_t dst, void *src_p, size_t size)
{
	struct apply_ctx *c = arg_p;
	const uint8_t *src = src_p;

	if (s_w.len > 0U && (uint32_t)dst != s_w.addr + (uint32_t)s_w.len) {
		if (wbuf_flush_all(c) != 0) {
			return -1;
		}
	}
	if (s_w.len == 0U) {
		s_w.addr = (uint32_t)dst;
	}

	while (size > 0U) {
		size_t n = MIN(size, WBUF_SZ - s_w.len);

		memcpy(s_w.buf + s_w.len, src, n);
		s_w.len += n;
		src += n;
		size -= n;

		if (s_w.len == WBUF_SZ && wbuf_flush_words(c) != 0) {
			return -1;
		}
	}

	return 0;
}

/**
 * Read bytes from the primary flash area for a patch segment by flushing any buffered writes first
 * and reading from the flash area; returns -1 on failure.
 */
static int mem_read(void *arg_p, void *dst_p, uintptr_t src, size_t size)
{
	struct apply_ctx *c = arg_p;

	/* Anything still buffered has to be in flash before it can be read back. */
	if (wbuf_flush_all(c) != 0) {
		return -1;
	}
	if (tinyfota_flash_read(c->primary, (uint32_t)src, dst_p, size) != 0) {
		DFU_LOG("r %u+%u failed\n", (unsigned)src, (unsigned)size);
		return -1;
	}
	return 0;
}

/**
 * Erase flash pages for a patch segment by rounding the size up to page boundaries, validating
 * alignment and bounds, and flushing any buffered writes first; returns -1 on failure.
 */
static int mem_erase(void *arg_p, uintptr_t addr, size_t size)
{
	struct apply_ctx *c = arg_p;
	size_t rounded;

	if (wbuf_flush_all(c) != 0) {
		return -1;
	}

	/*
	 * detools erases exactly the bytes a segment covers, and the LAST
	 * segment is a partial one -- 471 bytes for ultrawidelock's image. Flash erases
	 * pages, so that call fails outright.
	 *
	 * Rounding UP is safe, and not by luck. The forward shift is two
	 * segments, so the from-data segment k reads has been moved to start at
	 * exactly (k+1) * segment_size -- the first byte past the page this
	 * erase rounds up to. That is the property the shift exists to provide,
	 * and it is why the shift must stay a multiple of the segment size.
	 *
	 * Rounding the START down is never safe -- it would erase to-data
	 * already written by an earlier segment -- so an unaligned start is a
	 * hard failure rather than something to paper over.
	 */
	if ((addr % TINYFOTA_PAGE_SIZE) != 0U) {
		DFU_LOG("e %u unaligned\n", (unsigned)addr);
		return -1;
	}

	rounded = ROUND_UP(size, TINYFOTA_PAGE_SIZE);
	if (addr + rounded > tinyfota_flash_size(c->primary)) {
		DFU_LOG("e %u+%u past slot\n", (unsigned)addr, (unsigned)rounded);
		return -1;
	}

	if (tinyfota_flash_erase(c->primary, (uint32_t)addr, rounded) != 0) {
		DFU_LOG("e %u+%u failed\n", (unsigned)addr, (unsigned)rounded);
		return -1;
	}
	return 0;
}

/* ---- the step log -------------------------------------------------------- */
/*
 * detools calls step_set() after each completed segment and step_get() when it
 * wants to know where to resume. Steps start at 1 and increase by one, so step
 * N is recorded as a word at index N-1 and the number of written words IS the
 * last completed step.
 *
 * Append-only, never erased mid-apply: each word is written exactly once
 * between erases, which is the write pattern nRF flash is happiest with.
 */

/**
 * Record or clear the patch application step counter in the staging area: step 0 erases the page,
 * positive steps append a word, and out-of-range or negative steps fail; returns -1 on error.
 */
static int step_set(void *arg_p, int step)
{
	struct apply_ctx *c = arg_p;
	uint32_t value = (uint32_t)step;

	/*
	 * STEP 0 IS NOT A STEP. detools calls step_set(0) to CLEAR the counter,
	 * once every step is done -- in_place_all_steps_completed(),
	 * detools.c:1529-1541. The log is a page of append-only words, so
	 * clearing it means erasing that page.
	 *
	 * Rejecting it, which an earlier version of this file did, fails the
	 * patch at its very last moment, after all the work succeeded, and
	 * reports it as -DETOOLS_STEP_SET_FAILED rather than as anything that
	 * points here.
	 */
	if (step == 0) {
		if (tinyfota_flash_erase(c->staging, TINYFOTA_STEP_OFFSET,
					      TINYFOTA_PAGE_SIZE) != 0) {
			DFU_LOG("step clear failed\n");
			return -1;
		}
		return 0;
	}

	if (step < 0 || (size_t)step > STEP_MAX) {
		DFU_LOG("step %d out of range\n", step);
		return -1;
	}

	return tinyfota_flash_write(c->staging,
				TINYFOTA_STEP_OFFSET +
					((uint32_t)step - 1u) * (uint32_t)sizeof(value),
				&value, sizeof(value)) == 0
		       ? 0
		       : -1;
}

/**
 * Retrieve the current step counter from the staging area by reading append-only words until an
 * erased value is found; returns 0 on success.
 */
static int step_get(void *arg_p, int *step_p)
{
	struct apply_ctx *c = arg_p;
	uint32_t value;
	size_t i;

	for (i = 0; i < STEP_MAX; i++) {
		if (tinyfota_flash_read(c->staging,
				    TINYFOTA_STEP_OFFSET + (uint32_t)(i * sizeof(value)),
				    &value, sizeof(value)) != 0) {
			return -1;
		}
		if (value == TINYFOTA_STEP_ERASED) {
			break;
		}
	}

	*step_p = (int)i;
	return 0;
}

/* ---- integrity ----------------------------------------------------------- */

/**
 * CRC-32 over a run of a flash area.
 *
 * crc32_ieee_update() is seeded with 0 and chains across calls exactly as
 * Python's zlib.crc32(data, previous) does, which is what lets the host compute
 * the same value without either side reimplementing the other's polynomial.
 */
static int area_crc32(const struct tinyfota_flash_area *fa, uint32_t off, size_t len,
		      uint32_t *out)
{
	uint32_t crc = 0;

	while (len > 0U) {
		size_t n = MIN(sizeof(s_chunk), len);

		if (tinyfota_flash_read(fa, off, s_chunk, n) != 0) {
			return -1;
		}
		crc = tinyfota_crc32_update(crc, s_chunk, n);
		off += (uint32_t)n;
		len -= n;
	}

	*out = crc;
	return 0;
}

/** Erase the whole staging partition, consuming the update. */
static void staging_consume(struct apply_ctx *c)
{
	(void)tinyfota_flash_erase(c->staging, 0, tinyfota_flash_size(c->staging));
}

/* ---- the apply ----------------------------------------------------------- */

/**
 * Stream a patch from the staging area into detools in chunks, applying it in place to the primary
 * flash area, and flushing all buffered writes before finalization; returns detools result code.
 */
static int patch_stream(struct apply_ctx *c, const struct tinyfota_hdr *hdr)
{
	size_t left = hdr->patch_len;
	uint32_t off = TINYFOTA_PATCH_OFFSET;
	int res;

	s_w.len = 0U;
	s_w.addr = 0;

	res = detools_apply_patch_in_place_init(&s_patcher, mem_read, mem_write, mem_erase,
						step_set, step_get, hdr->patch_len, c);
	if (res != 0) {
		return res;
	}

	while (left > 0U) {
		size_t n = MIN(sizeof(s_chunk), left);

		if (tinyfota_flash_read(c->staging, off, s_chunk, n) != 0) {
			return -1;
		}

		res = detools_apply_patch_in_place_process(&s_patcher, s_chunk, n);
		if (res != 0) {
			return res;
		}

		off += (uint32_t)n;
		left -= n;
	}

	/* Nothing may stay in the combiner: detools has no idea it exists, so
	 * finalize() would report success over bytes still sitting in RAM. */
	if (wbuf_flush_all(c) != 0) {
		return -1;
	}

	return detools_apply_patch_in_place_finalize(&s_patcher);
}

/**
 * Apply a staged firmware delta to the primary flash partition during boot. Opens staging and
 * primary areas, validates header and patch CRC, applies patch in resumable steps, and erases
 * staging on completion or failure. Returns 0 always; actual success determined by primary image
 * validity on next boot.
 */
static int dfu_apply(void)
{
	struct tinyfota_hdr hdr;
	uint32_t crc;
	int completed = 0;
	int res;

	if (tinyfota_flash_open(TINYFOTA_FLASH_AREA_STAGING, &s_ctx.staging) != 0) {
		return 0;
	}

	/* The normal-boot fast path: no header, nothing staged, one read. */
	if (tinyfota_flash_read(s_ctx.staging, TINYFOTA_HDR_OFFSET, &hdr,
				     sizeof(hdr)) != 0 ||
	    hdr.magic != TINYFOTA_MAGIC) {
		tinyfota_flash_close(s_ctx.staging);
		return 0;
	}

	DFU_LOG("staged: len=%u to=%u\n", (unsigned)hdr.patch_len, (unsigned)hdr.to_len);

	if (hdr.abi_version != TINYFOTA_ABI_VERSION || hdr.patch_len == 0U ||
	    hdr.patch_len > tinyfota_flash_size(s_ctx.staging) - TINYFOTA_PATCH_OFFSET ||
	    hdr.hdr_crc32 != tinyfota_crc32((const uint8_t *)&hdr, TINYFOTA_HDR_CRC_LEN)) {
		DFU_LOG("header rejected\n");
		staging_consume(&s_ctx);
		tinyfota_flash_close(s_ctx.staging);
		return 0;
	}

	if (tinyfota_flash_open(TINYFOTA_FLASH_AREA_PRIMARY, &s_ctx.primary) != 0) {
		tinyfota_flash_close(s_ctx.staging);
		return 0;
	}

	if (step_get(&s_ctx, &completed) != 0) {
		goto done;
	}

	/*
	 * The from-image check is only meaningful on a FRESH apply. Once any
	 * step has completed the primary slot is half patched by design, so it
	 * no longer hashes to from_crc32 and re-checking it would reject every
	 * resume. That is also what makes the whole thing idempotent: a patch
	 * that already finished cannot match from_crc32 either, so a power cut
	 * between the last step and the erase below cannot re-apply it.
	 */
	if (completed == 0) {
		if (hdr.from_len > (uint32_t)tinyfota_flash_size(s_ctx.primary) ||
		    area_crc32(s_ctx.staging, TINYFOTA_PATCH_OFFSET, hdr.patch_len, &crc) !=
			    0 ||
		    crc != hdr.patch_crc32) {
			DFU_LOG("patch crc bad\n");
			staging_consume(&s_ctx);
			goto done;
		}

		if (area_crc32(s_ctx.primary, 0, hdr.from_len, &crc) != 0 ||
		    crc != hdr.from_crc32) {
			DFU_LOG("not for this image\n");
			staging_consume(&s_ctx);
			goto done;
		}
	}

	DFU_LOG("applying from step %d\n", completed);
	res = patch_stream(&s_ctx, &hdr);
	DFU_LOG("apply res=%d\n", res);

	/*
	 * Consume the staging partition either way. On success there is nothing
	 * left to do; on failure the primary slot is already damaged and
	 * retrying the same patch against it cannot help -- MCUboot's own
	 * validation will now fail and drop into serial recovery, which is the
	 * outcome that can actually be recovered from.
	 */
	staging_consume(&s_ctx);

done:
	tinyfota_flash_close(s_ctx.primary);
	tinyfota_flash_close(s_ctx.staging);
	return 0;
}

TINYFOTA_INIT_APPLICATION_PRIO(dfu_apply, 0);
