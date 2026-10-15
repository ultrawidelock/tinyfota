/* SPDX-License-Identifier: ISC */

/**
 * @file test_tinyfota.c — host checks for behaviour tinyfota added to the
 * imported core. Its own binary, so test_dfu.c and test_dfu_smp.c stay
 * check-for-check identical to the source suite they were imported from.
 *
 *   receiver base image   core/dfu_receiver.c refuses a signed patch whose
 *                         from_crc32 does not match the primary slot, after
 *                         the signature and before the first erase.
 *   applier fixes         two defects in core/dfu_applier.c the power-cut
 *                         simulator (tests/powercut/) found: a read-back that
 *                         left the next write unaligned, and a step recorded
 *                         before the bytes it claims were in flash.
 *
 * Same doubles as test_dfu.c: RAM flash with the nRF alignment rules, the
 * recording PSA (verify succeeds unless a knob says otherwise), and the
 * scripted detools.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "dfufake.h"
#include "psafake.h"
#include "test.h"

#include "dfu_crc.h"
#include "tinyfota.h"
#include "tinyfota/tinyfota_port.h"
#include "tinyfota_host.h"
#include "tinyfota_rx.h"

/* dfu_receiver.c links this from a generated file on target. The fake PSA
 * never looks at it. */
const uint8_t tinyfota_pubkey[65] = {0x04};

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define HEAD_LEN (TINYFOTA_HDR_LEN + TINYFOTA_SIG_LEN)
#define BASE_LEN 8192u

static const uint32_t kTransfer = 0x0badf00du;
static uint8_t s_base[BASE_LEN];

static void fill_pattern(uint8_t *dst, size_t len, uint8_t seed)
{
	for (size_t i = 0; i < len; i++) {
		dst[i] = (uint8_t)(seed + i * 13u);
	}
}

/** A self-consistent preamble claiming the given base. */
static void build_head(uint8_t *head, const uint8_t *patch, uint32_t patch_len, uint32_t from_len,
		       uint32_t from_crc)
{
	struct tinyfota_hdr hdr;

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = TINYFOTA_MAGIC;
	hdr.abi_version = TINYFOTA_ABI_VERSION;
	hdr.patch_len = patch_len;
	hdr.to_len = from_len;
	hdr.patch_crc32 = tinyfota_crc32(patch, patch_len);
	hdr.from_crc32 = from_crc;
	hdr.from_len = from_len;
	memcpy(head, &hdr, sizeof(hdr));
	hdr.hdr_crc32 = tinyfota_crc32(head, TINYFOTA_HDR_CRC_LEN);
	memcpy(head, &hdr, sizeof(hdr));
	memset(head + TINYFOTA_HDR_LEN, 0xa5, TINYFOTA_SIG_LEN);
}

static size_t frame(const uint8_t *req, size_t len, uint8_t *rsp)
{
	size_t rsp_len = 0;

	(void)tinyfota_rx_frame(TINYFOTA_OWNER_TEST, req, len, rsp, &rsp_len);
	return rsp_len;
}

static size_t send_begin(uint32_t total, uint8_t *rsp)
{
	uint8_t req[9] = {TINYFOTA_OP_BEGIN};

	tinyfota_put_le32(kTransfer, &req[1]);
	tinyfota_put_le32(total, &req[5]);
	return frame(req, sizeof(req), rsp);
}

static size_t send_data(uint32_t off, const uint8_t *payload, size_t len, uint8_t *rsp)
{
	uint8_t req[512];

	req[0] = TINYFOTA_OP_DATA;
	tinyfota_put_le32(kTransfer, &req[1]);
	tinyfota_put_le32(off, &req[5]);
	memcpy(req + 9, payload, len);
	return frame(req, len + 9u, rsp);
}

static size_t send_commit(uint8_t *rsp)
{
	uint8_t req[5] = {TINYFOTA_OP_COMMIT};

	tinyfota_put_le32(kTransfer, &req[1]);
	return frame(req, sizeof(req), rsp);
}

static int is_err(const uint8_t *rsp, size_t rsp_len, enum tinyfota_err code)
{
	return rsp_len == 2u && rsp[0] == TINYFOTA_RSP_ERR && rsp[1] == (uint8_t)code;
}

static int is_ok(const uint8_t *rsp, size_t rsp_len, uint32_t got)
{
	return rsp_len == 9u && rsp[0] == TINYFOTA_RSP_OK && tinyfota_get_le32(&rsp[1]) == kTransfer &&
	       tinyfota_get_le32(&rsp[5]) == got;
}

/** Window open, nothing in flight, a known image in the primary slot, and a
 * marker in staging so "untouched" can be checked byte for byte. */
static void ready(void)
{
	dfufake_reset();
	psafake_reset();
	tinyfota_rx_reset_all();
	tinyfota_window_open(1000);
	fill_pattern(s_base, sizeof(s_base), 0x31);
	memcpy(dfufake_primary.buf, s_base, sizeof(s_base));
	memset(dfufake_staging.buf, 0x5a, TINYFOTA_PAGE_SIZE);
}

/** Send the preamble and an 8-byte patch; return the reply to the DATA frame. */
static size_t push_head(uint32_t from_len, uint32_t from_crc, uint8_t *rsp)
{
	uint8_t patch[8];
	uint8_t buf[HEAD_LEN + sizeof(patch)];

	fill_pattern(patch, sizeof(patch), 0x77);
	build_head(buf, patch, sizeof(patch), from_len, from_crc);
	memcpy(buf + HEAD_LEN, patch, sizeof(patch));
	(void)send_begin(sizeof(buf), rsp);
	return send_data(0, buf, sizeof(buf), rsp);
}

static int staging_untouched(void)
{
	for (size_t i = 0; i < TINYFOTA_PAGE_SIZE; i++) {
		if (dfufake_peek(&dfufake_staging, i) != 0x5a) {
			return 0;
		}
	}
	return dfufake_staging.erase_calls == 0u;
}

static void test_receiver_base(void)
{
	uint8_t rsp[TINYFOTA_RSP_MAX];
	size_t n;

	t_group("receiver base image");

	/* The base the signer named is the one running: accepted, staged. */
	ready();
	n = push_head(BASE_LEN, tinyfota_crc32(s_base, BASE_LEN), rsp);
	T_OK("matching base: data accepted", is_ok(rsp, n, HEAD_LEN + 8u));
	T_EQ("matching base: staging erased once", (long)dfufake_staging.erase_calls, 1L);
	n = send_commit(rsp);
	T_OK("matching base: commit accepted", is_ok(rsp, n, HEAD_LEN + 8u));
	T_EQ("matching base: header landed", (long)tinyfota_get_le32(dfufake_staging.buf),
	     (long)TINYFOTA_MAGIC);

	/* A signed patch for another image: refused before anything is erased. */
	ready();
	n = push_head(BASE_LEN, tinyfota_crc32(s_base, BASE_LEN) ^ 1u, rsp);
	T_OK("wrong base: ERR_BASE", is_err(rsp, n, TINYFOTA_ERR_BASE));
	T_OK("wrong base: staging untouched", staging_untouched());
	T_EQ("wrong base: verified first", (long)psafake.verify_calls, 1L);
	n = send_commit(rsp);
	T_OK("wrong base: transfer dropped", is_err(rsp, n, TINYFOTA_ERR_SEQUENCE));

	/* One differing byte in the running image is a different image. */
	ready();
	dfufake_primary.buf[BASE_LEN - 1u] ^= 0x01u;
	n = push_head(BASE_LEN, tinyfota_crc32(s_base, BASE_LEN), rsp);
	T_OK("primary changed: ERR_BASE", is_err(rsp, n, TINYFOTA_ERR_BASE));

	/* A base longer than the slot cannot be the running image. */
	ready();
	n = push_head(DFUFAKE_PRIMARY_SIZE + 4u, 0, rsp);
	T_OK("base past the slot: ERR_BASE", is_err(rsp, n, TINYFOTA_ERR_BASE));
	T_OK("base past the slot: staging untouched", staging_untouched());

	/* The signature is checked first: an unsigned header never costs a
	 * read of the whole slot. */
	ready();
	psafake.verify_ret = -1;
	n = push_head(BASE_LEN, tinyfota_crc32(s_base, BASE_LEN) ^ 1u, rsp);
	T_OK("bad signature wins: ERR_AUTH", is_err(rsp, n, TINYFOTA_ERR_AUTH));
	T_EQ("bad signature wins: primary never opened", (long)dfufake_primary.open_calls, 0L);

	/* A slot that cannot be read is a port fault, reported as one. */
	ready();
	dfufake_primary.read_fail_in = 0;
	n = push_head(BASE_LEN, tinyfota_crc32(s_base, BASE_LEN), rsp);
	T_OK("unreadable primary: ERR_FLASH", is_err(rsp, n, TINYFOTA_ERR_FLASH));
	T_OK("unreadable primary: staging untouched", staging_untouched());
	T_EQ("unreadable primary: closed again", (long)dfufake_primary.close_calls,
	     (long)dfufake_primary.open_calls);

	ready();
	dfufake_primary.fail_open = 1;
	n = push_head(BASE_LEN, tinyfota_crc32(s_base, BASE_LEN), rsp);
	T_OK("primary will not open: ERR_FLASH", is_err(rsp, n, TINYFOTA_ERR_FLASH));
	T_OK("primary will not open: staging untouched", staging_untouched());

	/* from_len 0 names no base, which is what a whole-image payload says. */
	ready();
	n = push_head(0, 0, rsp);
	T_OK("no base named: accepted", is_ok(rsp, n, HEAD_LEN + 8u));

	/* The SMP path reaches the same check. */
	ready();
	{
		uint8_t patch[8];
		uint8_t buf[HEAD_LEN + sizeof(patch)];
		uint32_t next = 0;

		fill_pattern(patch, sizeof(patch), 0x77);
		build_head(buf, patch, sizeof(patch), BASE_LEN, tinyfota_crc32(s_base, BASE_LEN) ^ 1u);
		memcpy(buf + HEAD_LEN, patch, sizeof(patch));
		T_EQ("smp wrong base: refused",
		     tinyfota_rx_upload(0, sizeof(buf), buf, sizeof(buf), &next), -EINVAL);
		T_OK("smp wrong base: staging untouched", staging_untouched());
		T_OK("smp wrong base: nothing staged", !tinyfota_rx_staged());
	}
}

/* ---- applier -------------------------------------------------------------- */

extern int (*const tinyfota_init_dfu_apply)(void);

#define APPLY_FROM_LEN 4096u

/** A valid update in staging over a known primary, nothing recorded yet. */
static void stage_apply(void)
{
	uint8_t patch[16];
	uint8_t head[HEAD_LEN];

	dfufake_reset();
	fill_pattern(dfufake_primary.buf, APPLY_FROM_LEN, 0x21);
	fill_pattern(patch, sizeof(patch), 17);
	build_head(head, patch, sizeof(patch), APPLY_FROM_LEN,
		   tinyfota_crc32(dfufake_primary.buf, APPLY_FROM_LEN));
	memcpy(dfufake_staging.buf, head, TINYFOTA_HDR_LEN);
	memcpy(dfufake_staging.buf + TINYFOTA_PATCH_OFFSET, patch, sizeof(patch));
}

static int bytes_are(const struct tinyfota_flash_host_area *a, size_t off, size_t len, uint8_t v)
{
	for (size_t i = 0; i < len; i++) {
		if (dfufake_peek(a, off + i) != v) {
			return 0;
		}
	}
	return 1;
}

static void test_applier_fixes(void)
{
	t_group("applier fixes");

	/* A read-back over a partial word sees the held bytes and leaves them
	 * held: flash is not padded under detools' feet. */
	{
		const struct dfufake_op ops[] = {
			{DFUFAKE_OP_ERASE, 0, DFUFAKE_PAGE_SIZE, 0, 0},
			{DFUFAKE_OP_WRITE, 0, 3, 0, 0x11},
			{DFUFAKE_OP_READ, 0, 8, 0, 0},
		};
		const uint8_t *got;

		stage_apply();
		dfufake_script(ops, ARRAY_SIZE(ops));
		T_EQ("read-back: apply returns 0", tinyfota_init_dfu_apply(), 0);
		got = dfufake_read_bytes();
		T_OK("read-back: held bytes visible", got[0] == 0x11 && got[2] == 0x11);
		T_OK("read-back: erased past them", got[3] == 0xff && got[7] == 0xff);
		T_EQ("read-back: finalized", (long)dfufake.detools_finalize_calls, 1L);
	}

	/* The write that follows the read-back continues the held word. As
	 * imported, the read padded byte 3 with 0xff and this write started
	 * mid-word, which flash refuses. */
	{
		const struct dfufake_op ops[] = {
			{DFUFAKE_OP_ERASE, 0, DFUFAKE_PAGE_SIZE, 0, 0},
			{DFUFAKE_OP_WRITE, 0, 3, 0, 0x11},
			{DFUFAKE_OP_READ, 0, 4, 0, 0},
			{DFUFAKE_OP_WRITE, 3, 5, 0, 0x22},
		};

		stage_apply();
		dfufake_script(ops, ARRAY_SIZE(ops));
		T_EQ("write after read-back: apply returns 0", tinyfota_init_dfu_apply(), 0);
		T_EQ("write after read-back: every op accepted", (long)dfufake_ops_done(),
		     (long)ARRAY_SIZE(ops));
		T_OK("write after read-back: first bytes", bytes_are(&dfufake_primary, 0, 3, 0x11));
		T_OK("write after read-back: continued", bytes_are(&dfufake_primary, 3, 5, 0x22));
		T_EQ("write after read-back: finalized", (long)dfufake.detools_finalize_calls, 1L);
	}

	/* A region that starts mid-word is written from the word boundary, the
	 * lead bytes programmed as 0xff. */
	{
		const struct dfufake_op ops[] = {
			{DFUFAKE_OP_ERASE, 0, DFUFAKE_PAGE_SIZE, 0, 0},
			{DFUFAKE_OP_WRITE, 1, 3, 0, 0x33},
		};

		stage_apply();
		dfufake_script(ops, ARRAY_SIZE(ops));
		T_EQ("unaligned start: apply returns 0", tinyfota_init_dfu_apply(), 0);
		T_EQ("unaligned start: finalized", (long)dfufake.detools_finalize_calls, 1L);
		T_EQ("unaligned start: lead byte erased", (long)dfufake_peek(&dfufake_primary, 0),
		     0xffL);
		T_OK("unaligned start: landed", bytes_are(&dfufake_primary, 1, 3, 0x33));
		T_EQ("unaligned start: written at the word", (long)dfufake_primary.last_write_off, 0L);
	}

	/* A step is recorded only after the bytes it covers are in flash. With
	 * the primary refusing writes, the held tail cannot land, so nothing
	 * may reach the step log. */
	{
		const struct dfufake_op ops[] = {
			{DFUFAKE_OP_WRITE, 0, 3, 0, 0x44},
			{DFUFAKE_OP_STEP_SET, 0, 0, 1, 0},
		};

		stage_apply();
		dfufake_script(ops, ARRAY_SIZE(ops));
		dfufake_primary.write_fail_in = 0;
		T_EQ("step after failed flush: apply returns 0", tinyfota_init_dfu_apply(), 0);
		T_EQ("step after failed flush: stopped at the step", (long)dfufake_ops_done(), 2L);
		T_EQ("step after failed flush: step log untouched",
		     (long)dfufake_staging.write_calls, 0L);
	}

	/* The same for the final clear: the log is not erased while the last
	 * bytes are still in RAM. The one staging erase is the consume. */
	{
		const struct dfufake_op ops[] = {
			{DFUFAKE_OP_WRITE, 0, 3, 0, 0x44},
			{DFUFAKE_OP_STEP_SET, 0, 0, 0, 0},
		};

		stage_apply();
		dfufake_script(ops, ARRAY_SIZE(ops));
		dfufake_primary.write_fail_in = 0;
		T_EQ("clear after failed flush: apply returns 0", tinyfota_init_dfu_apply(), 0);
		T_EQ("clear after failed flush: stopped at the clear", (long)dfufake_ops_done(), 2L);
		T_EQ("clear after failed flush: only the consume erased",
		     (long)dfufake_staging.erase_calls, 1L);
		T_EQ("clear after failed flush: consume was whole-partition",
		     (long)dfufake_staging.last_erase_len, (long)DFUFAKE_STAGING_SIZE);
	}

	/* The happy path still records and clears. */
	{
		const struct dfufake_op ops[] = {
			{DFUFAKE_OP_ERASE, 0, DFUFAKE_PAGE_SIZE, 0, 0},
			{DFUFAKE_OP_WRITE, 0, 3, 0, 0x55},
			{DFUFAKE_OP_STEP_SET, 0, 0, 1, 0},
			{DFUFAKE_OP_STEP_GET, 0, 0, 0, 0},
		};

		stage_apply();
		dfufake_script(ops, ARRAY_SIZE(ops));
		T_EQ("step after flush: apply returns 0", tinyfota_init_dfu_apply(), 0);
		T_EQ("step after flush: recorded", dfufake_last_step_get(), 1);
		T_EQ("step after flush: one word in the log", (long)dfufake_staging.write_calls, 1L);
	}
}

int main(void)
{
	test_receiver_base();
	test_applier_fixes();

	if (t_fail > 0) {
		printf("  tinyfota: FAIL (%d of %d)\n", t_fail, t_fail + t_pass);
		return 1;
	}
	printf("  tinyfota: PASS (%d checks — behaviour added to the imported core)\n", t_pass);
	return 0;
}
