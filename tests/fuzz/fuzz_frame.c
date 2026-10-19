/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file fuzz_frame.c - the receiver's frame path, as any peer in radio range
 * reaches it.
 *
 * Input: one byte of header fix-up flags (fuzz_port.h; applied to the first
 * 32 B of any DATA frame at offset 0), then records:
 *
 *   [owner] [len] [len bytes]   one frame from transport owner % 5 (NONE is
 *                               included: a transport bug could pass it)
 *   [0x80|n] [m]                the clock moves (n * 256 + m) * 100 ms, so
 *                               the window and the idle timer can expire
 *                               between frames
 *
 * Every reply must fit TINYFOTA_RSP_MAX. Whatever ends up staged is booted.
 */
#include <stdint.h>
#include <string.h>

#include "fuzz_port.h"
#include "tinyfota_host.h"
#include "tinyfota_rx.h"

extern int (*const tinyfota_init_dfu_apply)(void);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint8_t req[256];
	unsigned fix;
	size_t i = 1;

	if (size < 1) {
		return 0;
	}
	fix = data[0] & 0x0bu; /* the patch CRC needs the whole patch: never fixed here */
	fuzz_reset();

	while (i + 2 <= size) {
		uint8_t a = data[i], b = data[i + 1];

		i += 2;
		if (a & 0x80u) {
			(void)tinyfota_osal_host_advance_ms((int64_t)(((a & 0x7fu) << 8) | b) * 100);
			continue;
		}

		size_t len = b;
		uint8_t rsp[TINYFOTA_RSP_MAX + 16];
		size_t rsp_len = 0;

		if (len > size - i) {
			len = size - i;
		}
		memcpy(req, data + i, len);
		i += len;
		if (len >= 9 + TINYFOTA_HDR_LEN && req[0] == TINYFOTA_OP_DATA &&
		    tinyfota_get_le32(&req[5]) == 0) {
			fuzz_fix_header(req + 9, fix, NULL, 0);
		}
		(void)tinyfota_rx_frame((enum tinyfota_owner)(a % 5u), req, len, rsp, &rsp_len);
		if (rsp_len > TINYFOTA_RSP_MAX) {
			__builtin_trap();
		}
	}

	if (tinyfota_rx_staged()) {
		(void)tinyfota_init_dfu_apply();
	}
	return 0;
}
