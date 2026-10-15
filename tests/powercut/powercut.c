/* SPDX-License-Identifier: ISC */

/**
 * @file powercut.c — cut the power at EVERY flash operation of a real update.
 *
 *   build/host/powercut <base.bin> <target.bin> <update.wdfu>
 *
 * What runs is what ships: core/dfu_receiver.c stages the .wdfu exactly as it
 * arrives over the air, then core/dfu_applier.c applies it with the vendored
 * detools, compiled with the bootloader's own settings. Only the flash is a
 * model, and it is a strict one:
 *
 *   - an erase sets a 4,096 B page to 0xff, and a multi-page erase is one
 *     operation PER PAGE, each atomic;
 *   - a write can only clear bits (the stored word is ANDed with the data),
 *     must be 4 B aligned, and is atomic per 4 B word;
 *   - every page erase and every write call is one numbered operation.
 *
 * For every operation k of the whole update -- staging and applying -- the
 * harness starts from the base image, runs the update with the power cut at
 * operation k, and then reboots: the applier runs again, uncut, as MCUboot's
 * SYS_INIT would, and once more after that, as the next boot would. Two kinds
 * of cut are tried at every k:
 *
 *   lost   operation k does not happen at all;
 *   torn   operation k half happens: the first half of a write's words land,
 *          or for an erase, nothing (an erase page is atomic in this model).
 *
 * The verdict for every run is one of exactly two outcomes. If the cut came
 * before the staged header had fully landed, the next boot must find nothing
 * to do and the primary slot must still hold the base image, byte for byte.
 * Otherwise the primary slot must end up holding the target image, byte for
 * byte over its length. Anything else -- a half-patched slot, a patch applied
 * twice, an applier that gives up -- fails the run.
 */

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyfota.h"
#include "tinyfota/tinyfota_port.h"
#include "tinyfota_rx.h"

/* The sample's partition map (samples/nrf52833/pm_static.yml). */
#define PRIMARY_SIZE 0x6a000u
#define STAGING_SIZE 0x8000u
#define PAGE         4096u
#define WORD         4u

/* ---- the flash model ------------------------------------------------------- */

struct tinyfota_flash_area {
	uint8_t *buf;
	size_t size;
};

static uint8_t g_primary[PRIMARY_SIZE];
static uint8_t g_staging[STAGING_SIZE];
static struct tinyfota_flash_area g_areas[2] = {
	[TINYFOTA_FLASH_AREA_PRIMARY] = {g_primary, sizeof(g_primary)},
	[TINYFOTA_FLASH_AREA_STAGING] = {g_staging, sizeof(g_staging)},
};

enum cut_kind { CUT_LOST, CUT_TORN };

static unsigned long g_ops;      /* operations so far in this run */
static unsigned long g_cut_at;   /* 0 = never */
static enum cut_kind g_cut_kind;
static jmp_buf g_power;          /* where a cut lands */
static unsigned long g_hdr_op;   /* the op that wrote the staged header */
static unsigned long g_violations;

static bool g_trace; /* POWERCUT_TRACE: print every operation of the uncut run */

/* Count one operation; true when this is the one the power dies in. */
static bool op_is_cut(void)
{
	g_ops++;
	return g_cut_at != 0 && g_ops == g_cut_at;
}

static void trace(const char *what, const struct tinyfota_flash_area *fa, uint32_t off,
		  size_t len)
{
	if (g_trace) {
		printf("  op %5lu  %-5s %-7s +0x%05x %5zu B\n", g_ops, what,
		       fa->buf == g_primary ? "primary" : "staging", (unsigned)off, len);
	}
}

int tinyfota_flash_open(enum tinyfota_flash_area_id id, const struct tinyfota_flash_area **fa)
{
	*fa = &g_areas[id];
	return 0;
}

void tinyfota_flash_close(const struct tinyfota_flash_area *fa)
{
	(void)fa;
}

size_t tinyfota_flash_size(const struct tinyfota_flash_area *fa)
{
	return fa->size;
}

int tinyfota_flash_read(const struct tinyfota_flash_area *fa, uint32_t off, void *dst, size_t len)
{
	if (off > fa->size || len > fa->size - off) {
		return -1;
	}
	memcpy(dst, fa->buf + off, len);
	return 0;
}

static void program(uint8_t *dst, const uint8_t *src, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		/* A NOR cell can only go from 1 to 0 without an erase. A write
		 * that asks for a 0 to become a 1 is a bug in the caller: count it,
		 * and store what real flash would hold. */
		if ((uint8_t)(src[i] & ~dst[i]) != 0U) {
			g_violations++;
		}
		dst[i] &= src[i];
	}
}

int tinyfota_flash_write(const struct tinyfota_flash_area *fa, uint32_t off, const void *src,
			 size_t len)
{
	if ((off % WORD) != 0U || (len % WORD) != 0U || off > fa->size || len > fa->size - off) {
		return -1;
	}
	if (fa == &g_areas[TINYFOTA_FLASH_AREA_STAGING] && off == TINYFOTA_HDR_OFFSET &&
	    len == TINYFOTA_HDR_LEN) {
		g_hdr_op = g_ops + 1u;
	}
	if (op_is_cut()) {
		trace("write", fa, off, len);
		if (g_cut_kind == CUT_TORN) {
			program(fa->buf + off, src, (len / WORD / 2u) * WORD);
		}
		longjmp(g_power, 1);
	}
	trace("write", fa, off, len);
	program(fa->buf + off, src, len);
	return 0;
}

int tinyfota_flash_erase(const struct tinyfota_flash_area *fa, uint32_t off, size_t len)
{
	if ((off % PAGE) != 0U || (len % PAGE) != 0U || off > fa->size || len > fa->size - off) {
		return -1;
	}
	for (size_t p = 0; p < len; p += PAGE) {
		if (op_is_cut()) {
			longjmp(g_power, 1); /* this page, and the rest, untouched */
		}
		trace("erase", fa, (uint32_t)(off + p), PAGE);
		memset(fa->buf + off + p, 0xff, PAGE);
	}
	return 0;
}

/* ---- the rest of the receiver's seams ----------------------------------------- */

static unsigned g_reboots;

void tinyfota_reboot(void)
{
	g_reboots++;
}

int64_t tinyfota_uptime_ms(void)
{
	return 0;
}

void tinyfota_lock(void)
{
}

void tinyfota_unlock(void)
{
}

void tinyfota_timer_init(enum tinyfota_timer t, tinyfota_timer_fn fn)
{
	(void)t;
	(void)fn;
}

int tinyfota_timer_schedule(enum tinyfota_timer t, int32_t delay_ms)
{
	(void)t;
	(void)delay_ms;
	return 0;
}

int tinyfota_timer_reschedule(enum tinyfota_timer t, int32_t delay_ms)
{
	(void)t;
	(void)delay_ms;
	return 0;
}

int tinyfota_timer_cancel(enum tinyfota_timer t)
{
	(void)t;
	return 0;
}

/* The signature is not under test here; the receiver's suite covers it. */
int tinyfota_ecdsa_p256_verify(const uint8_t pub[65], const uint8_t *msg, size_t msg_len,
			       const uint8_t sig[64])
{
	(void)pub;
	(void)msg;
	(void)msg_len;
	(void)sig;
	return 0;
}

const uint8_t tinyfota_pubkey[65] = {0x04};

/* The optional log seam: silent unless POWERCUT_VERBOSE is set, so a failing
 * run can be replayed with the receiver's and the applier's own narration. */
#include <stdarg.h>
void tinyfota_log(int level, const char *fmt, ...)
{
	va_list ap;

	(void)level;
	if (getenv("POWERCUT_VERBOSE") == NULL) {
		return;
	}
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if (fmt[0] != '\0' && fmt[strlen(fmt) - 1] != '\n') {
		putchar('\n');
	}
}

/* dfu_applier.c's entry point, exposed by TINYFOTA_INIT_APPLICATION_PRIO. */
extern int (*const tinyfota_init_dfu_apply)(void);

/* ---- one update ------------------------------------------------------------- */

static uint8_t *g_base, *g_target, *g_wdfu;
static size_t g_base_len, g_target_len, g_wdfu_len;

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long n;

	if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fprintf(stderr, "powercut: cannot read %s\n", path);
		exit(2);
	}
	buf = malloc((size_t)n);
	if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "powercut: cannot read %s\n", path);
		exit(2);
	}
	fclose(f);
	*len = (size_t)n;
	return buf;
}

static void frame(const uint8_t *req, size_t len)
{
	uint8_t rsp[TINYFOTA_RSP_MAX];
	size_t rsp_len = 0;

	(void)tinyfota_rx_frame(TINYFOTA_OWNER_TEST, req, len, rsp, &rsp_len);
}

/* Stage the .wdfu through the receiver, as the radio would deliver it: BEGIN,
 * 244 B DATA frames, COMMIT. */
static void stage(void)
{
	uint8_t req[9 + 244];
	const uint32_t id = 0x5eed5eedu;

	req[0] = TINYFOTA_OP_BEGIN;
	tinyfota_put_le32(id, &req[1]);
	tinyfota_put_le32((uint32_t)g_wdfu_len, &req[5]);
	frame(req, 9);
	for (size_t off = 0; off < g_wdfu_len; off += 244u) {
		size_t n = g_wdfu_len - off < 244u ? g_wdfu_len - off : 244u;

		req[0] = TINYFOTA_OP_DATA;
		tinyfota_put_le32(id, &req[1]);
		tinyfota_put_le32((uint32_t)off, &req[5]);
		memcpy(req + 9, g_wdfu + off, n);
		frame(req, 9 + n);
	}
	req[0] = TINYFOTA_OP_COMMIT;
	tinyfota_put_le32(id, &req[1]);
	frame(req, 5);
}

/* A fresh board running the base image, with nothing staged. */
static void board_reset(void)
{
	memset(g_primary, 0xff, sizeof(g_primary));
	memcpy(g_primary, g_base, g_base_len);
	memset(g_staging, 0xff, sizeof(g_staging));
	tinyfota_rx_reset_all();
	tinyfota_window_open(300000);
	g_ops = 0;
	g_hdr_op = 0;
	g_reboots = 0;
}

enum outcome { OUT_TARGET, OUT_BASE, OUT_OTHER };

static enum outcome slot_state(void)
{
	if (memcmp(g_primary, g_target, g_target_len) == 0) {
		return OUT_TARGET;
	}
	if (memcmp(g_primary, g_base, g_base_len) == 0) {
		bool rest_erased = true;

		for (size_t i = g_base_len; i < sizeof(g_primary); i++) {
			if (g_primary[i] != 0xff) {
				rest_erased = false;
				break;
			}
		}
		return rest_erased ? OUT_BASE : OUT_OTHER;
	}
	return OUT_OTHER;
}

/*
 * One whole update with the power cut at operation @p cut_at (0 = no cut):
 * stage, boot, and two more boots. Returns the slot's final state. *@p cut_in
 * says where the cut fell: 0 staging, 1 the first boot's apply, 2 never.
 */
static enum outcome run(unsigned long cut_at, enum cut_kind kind, int *cut_in,
			unsigned long *hdr_op)
{
	volatile int phase = 0;

	board_reset();
	g_cut_at = cut_at;
	g_cut_kind = kind;
	*cut_in = 2;

	if (setjmp(g_power) == 0) {
		stage();
		phase = 1;
		(void)tinyfota_init_dfu_apply();
	} else {
		*cut_in = phase;
	}
	*hdr_op = g_hdr_op;

	/* The power comes back. Two boots, uncut: the first finishes whatever
	 * was interrupted, the second must find nothing left to do. */
	g_cut_at = 0;
	tinyfota_rx_reset_all();
	(void)tinyfota_init_dfu_apply();
	(void)tinyfota_init_dfu_apply();

	/* Nothing may stay staged: an update the applier neither finished nor
	 * consumed would run again at every boot. */
	if (tinyfota_get_le32(g_staging + TINYFOTA_HDR_OFFSET) == TINYFOTA_MAGIC) {
		return OUT_OTHER;
	}
	return slot_state();
}

int main(int argc, char **argv)
{
	unsigned long total, hdr_op;
	unsigned long failures = 0, runs = 0, base_runs = 0, target_runs = 0;
	unsigned long in_staging = 0, in_apply = 0;
	int cut_in;

	if (argc != 4) {
		fprintf(stderr, "usage: powercut <base.bin> <target.bin> <update.wdfu>\n");
		return 2;
	}
	g_base = slurp(argv[1], &g_base_len);
	g_target = slurp(argv[2], &g_target_len);
	g_wdfu = slurp(argv[3], &g_wdfu_len);
	if (g_base_len > PRIMARY_SIZE || g_target_len > PRIMARY_SIZE) {
		fprintf(stderr, "powercut: an image is larger than the primary slot\n");
		return 2;
	}

	/* The uncut run first: it must succeed, and it says how many
	 * operations there are to cut. */
	g_trace = getenv("POWERCUT_TRACE") != NULL;
	if (run(0, CUT_LOST, &cut_in, &hdr_op) != OUT_TARGET || g_violations != 0) {
		fprintf(stderr, "powercut: the update does not apply even without a cut "
				"(%lu bit violations)\n", g_violations);
		return 1;
	}
	total = g_ops;
	g_trace = false;
	printf("  update: %zu B base -> %zu B target, %zu B .wdfu\n", g_base_len, g_target_len,
	       g_wdfu_len);
	printf("  %lu flash operations uncut (staged header lands at op %lu)\n", total, hdr_op);
	fflush(stdout);

	for (int k = 0; k < 2; k++) {
		enum cut_kind kind = k == 0 ? CUT_LOST : CUT_TORN;

		for (unsigned long at = 1; at <= total; at++) {
			unsigned long hdr;
			enum outcome got = run(at, kind, &cut_in, &hdr);
			/* The header lands at op hdr_op; a cut AT that op loses or
			 * tears it, so only a cut after it may produce the target. */
			enum outcome want = at > hdr_op ? OUT_TARGET : OUT_BASE;

			runs++;
			if (cut_in == 0) {
				in_staging++;
			} else if (cut_in == 1) {
				in_apply++;
			}
			if (got == OUT_TARGET) {
				target_runs++;
			} else if (got == OUT_BASE) {
				base_runs++;
			}
			if (got != want) {
				failures++;
				if (failures <= 20) {
					printf("  FAIL  %s cut at op %lu: slot holds %s, want %s\n",
					       kind == CUT_LOST ? "lost" : "torn", at,
					       got == OUT_TARGET ? "the target"
					       : got == OUT_BASE ? "the base"
								 : "neither image",
					       want == OUT_TARGET ? "the target" : "the base");
				}
			}
		}
	}

	printf("  %lu runs (2 kinds x %lu cut points): %lu ended on the target, %lu on the "
	       "untouched base\n", runs, total, target_runs, base_runs);
	printf("  cuts during staging %lu, during the apply %lu\n", in_staging, in_apply);
	if (g_violations != 0) {
		printf("  FAIL  %lu writes asked flash to set a bit without an erase\n",
		       g_violations);
		failures++;
	}
	if (failures != 0) {
		printf("  powercut: FAIL (%lu)\n", failures);
		return 1;
	}
	printf("  powercut: PASS, every cut point ends on the target or the untouched base\n");
	return 0;
}
