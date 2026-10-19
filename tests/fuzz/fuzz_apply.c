/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file fuzz_apply.c - patch application on garbage.
 *
 * Every check a patch has to pass before detools sees it is a CRC, so a
 * corrupted or hostile patch that was signed (a leaked key, a buggy signer)
 * reaches the vendored detools with the bootloader's settings. This feeds it
 * one: the header is made valid for a known base image, and the patch bytes
 * are the fuzzer's.
 *
 * Input: [to_len:4 LE] [steps:1] [patch...]. steps words of a step log are
 * written first, so the resume path parses garbage too. The applier then
 * boots twice, as after a power cut. What must hold: no sanitizer report, no
 * hang, and no flash access outside the two partitions (the host port
 * refuses those, so detools sees an error rather than corrupting memory).
 *
 * The base is FUZZ_BASE_LEN bytes of the pattern in base_byte(), which
 * gen_seeds.py reproduces to make real patches for the seed corpus.
 */
#include <stdint.h>
#include <string.h>

#include "dfu_crc.h"
#include "fuzz_port.h"
#include "tinyfota_host.h"

extern int (*const tinyfota_init_dfu_apply)(void);

#define FUZZ_BASE_LEN 16384u

static uint8_t base_byte(uint32_t i)
{
	return (uint8_t)((i * 2654435761u) >> 24);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct tinyfota_flash_host_area *p = tinyfota_flash_host_area(TINYFOTA_FLASH_AREA_PRIMARY);
	struct tinyfota_flash_host_area *s = tinyfota_flash_host_area(TINYFOTA_FLASH_AREA_STAGING);
	uint8_t hdr[TINYFOTA_HDR_LEN];
	size_t patch_len;
	unsigned steps;

	if (size < 5) {
		return 0;
	}
	patch_len = size - 5;
	if (patch_len > s->size - TINYFOTA_PATCH_OFFSET) {
		patch_len = s->size - TINYFOTA_PATCH_OFFSET;
	}
	if (patch_len == 0) {
		return 0;
	}
	steps = data[4];

	fuzz_reset();
	for (uint32_t i = 0; i < FUZZ_BASE_LEN; i++) {
		p->buf[i] = base_byte(i);
	}

	memset(hdr, 0, sizeof(hdr));
	tinyfota_put_le32(tinyfota_get_le32(data), &hdr[12]); /* to_len */
	tinyfota_put_le32(tinyfota_crc32(p->buf, FUZZ_BASE_LEN), &hdr[20]);
	tinyfota_put_le32(FUZZ_BASE_LEN, &hdr[24]);
	fuzz_fix_header(hdr, 0x0fu, data + 5, patch_len);

	memcpy(s->buf + TINYFOTA_HDR_OFFSET, hdr, sizeof(hdr));
	memcpy(s->buf + TINYFOTA_PATCH_OFFSET, data + 5, patch_len);
	for (uint32_t k = 0; k < steps; k++) {
		tinyfota_put_le32(k + 1u, s->buf + TINYFOTA_STEP_OFFSET + 4u * k);
	}

	(void)tinyfota_init_dfu_apply();
	(void)tinyfota_init_dfu_apply();
	return 0;
}
