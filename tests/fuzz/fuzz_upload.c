/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file fuzz_upload.c - the receiver's SMP upload path (tinyfota_rx_upload),
 * which a stock mcumgr client drives with whatever offsets it likes.
 *
 * Input: one byte of header fix-up flags (fuzz_port.h; applied to the first
 * 32 B of any chunk at offset 0), then records:
 *
 *   [off:4 LE] [total:4 LE] [len:2 LE] [len bytes]   one upload chunk
 *
 * A record whose off has its top bit set instead moves the clock by
 * (off & 0xffff) * 100 ms. Whatever ends up staged is booted. The oracle is
 * the sanitizers: the next offset is not checked against total, because an
 * off-0 chunk mid-upload is a resync answered with the real cursor (its
 * total ignored), and trailer bytes past a wrapped patch are acknowledged.
 */
#include <stdint.h>
#include <string.h>

#include "fuzz_port.h"
#include "tinyfota_host.h"
#include "tinyfota_rx.h"

extern int (*const tinyfota_init_dfu_apply)(void);

static uint8_t g_chunk[0x10000];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	unsigned fix;
	size_t i = 1;

	if (size < 1) {
		return 0;
	}
	fix = data[0] & 0x0bu;
	fuzz_reset();

	while (i + 10 <= size) {
		uint32_t off = tinyfota_get_le32(data + i);
		uint32_t total = tinyfota_get_le32(data + i + 4);
		size_t len = (size_t)data[i + 8] | ((size_t)data[i + 9] << 8);
		uint32_t next = 0;

		i += 10;
		if (off & 0x80000000u) {
			(void)tinyfota_osal_host_advance_ms((int64_t)(off & 0xffffu) * 100);
			continue;
		}
		if (len > size - i) {
			len = size - i;
		}
		memcpy(g_chunk, data + i, len);
		i += len;
		if (off == 0 && len >= TINYFOTA_HDR_LEN) {
			fuzz_fix_header(g_chunk, fix, NULL, 0);
		}
		(void)tinyfota_rx_upload(off, total, g_chunk, len, &next);
	}

	if (tinyfota_rx_staged()) {
		(void)tinyfota_init_dfu_apply();
	}
	return 0;
}
