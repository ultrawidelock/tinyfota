/* SPDX-License-Identifier: ISC */

/**
 * @file fuzz_header.c - the 32 B header, through both of its parsers.
 *
 * Input: one byte of fix-up flags (fuzz_fix_header(), plus bit 4: make
 * from_crc32 match the primary slot), then the preamble (header and
 * signature) and patch as a host would send them.
 *
 *   1. The receiver takes it as BEGIN, 244 B DATA frames and COMMIT, and the
 *      applier boots on whatever that staged.
 *   2. The applier boots on the same header written straight into staging,
 *      as flash could hold it after a fault the receiver never saw.
 *
 * Nothing is asserted beyond the sanitizers and the reply size: the header is
 * untrusted until its signature verifies, so every byte of it must be safe to
 * parse.
 */
#include <stdint.h>
#include <string.h>

#include "dfu_crc.h"
#include "fuzz_port.h"
#include "tinyfota_host.h"
#include "tinyfota_rx.h"

extern int (*const tinyfota_init_dfu_apply)(void);

#define PREAMBLE (TINYFOTA_HDR_LEN + TINYFOTA_SIG_LEN)
#define MAX_IN   (TINYFOTA_FLASH_HOST_STAGING_SIZE)

static uint8_t g_in[MAX_IN];

static void frame(const uint8_t *req, size_t len)
{
	uint8_t rsp[TINYFOTA_RSP_MAX + 16];
	size_t rsp_len = 0;

	(void)tinyfota_rx_frame(TINYFOTA_OWNER_TEST, req, len, rsp, &rsp_len);
	if (rsp_len > TINYFOTA_RSP_MAX) {
		__builtin_trap();
	}
}

static void fix_from(uint8_t *hdr)
{
	struct tinyfota_flash_host_area *p = tinyfota_flash_host_area(TINYFOTA_FLASH_AREA_PRIMARY);
	uint32_t from_len = tinyfota_get_le32(&hdr[24]);

	if (from_len > p->size) {
		from_len = (uint32_t)p->size;
		tinyfota_put_le32(from_len, &hdr[24]);
	}
	tinyfota_put_le32(tinyfota_crc32(p->buf, from_len), &hdr[20]);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const uint32_t id = 0x46555a5au;
	uint8_t req[9 + 244];
	unsigned fix;
	size_t len;

	if (size < 1) {
		return 0;
	}
	fix = data[0];
	len = size - 1 > MAX_IN ? MAX_IN : size - 1;
	memcpy(g_in, data + 1, len);
	if (len >= TINYFOTA_HDR_LEN) {
		const uint8_t *patch = len > PREAMBLE ? g_in + PREAMBLE : g_in;
		size_t patch_len = len > PREAMBLE ? len - PREAMBLE : 0;

		if (fix & 16u) {
			fix_from(g_in);
		}
		fuzz_fix_header(g_in, fix, patch, patch_len);
	}

	/* 1. Over the air, then a boot. */
	fuzz_reset();
	req[0] = TINYFOTA_OP_BEGIN;
	tinyfota_put_le32(id, &req[1]);
	tinyfota_put_le32((uint32_t)len, &req[5]);
	frame(req, 9);
	for (size_t off = 0; off < len; off += 244u) {
		size_t n = len - off < 244u ? len - off : 244u;

		req[0] = TINYFOTA_OP_DATA;
		tinyfota_put_le32(id, &req[1]);
		tinyfota_put_le32((uint32_t)off, &req[5]);
		memcpy(req + 9, g_in + off, n);
		frame(req, 9 + n);
	}
	req[0] = TINYFOTA_OP_COMMIT;
	tinyfota_put_le32(id, &req[1]);
	frame(req, 5);
	(void)tinyfota_rx_staged();
	(void)tinyfota_init_dfu_apply();

	/* 2. Straight into staging, past the receiver. */
	if (len >= TINYFOTA_HDR_LEN) {
		struct tinyfota_flash_host_area *s =
			tinyfota_flash_host_area(TINYFOTA_FLASH_AREA_STAGING);
		size_t patch_len = len > PREAMBLE ? len - PREAMBLE : 0;

		fuzz_reset();
		memcpy(s->buf + TINYFOTA_HDR_OFFSET, g_in, TINYFOTA_HDR_LEN);
		if (patch_len > s->size - TINYFOTA_PATCH_OFFSET) {
			patch_len = s->size - TINYFOTA_PATCH_OFFSET;
		}
		memcpy(s->buf + TINYFOTA_PATCH_OFFSET, g_in + PREAMBLE, patch_len);
		(void)tinyfota_init_dfu_apply();
	}
	return 0;
}
