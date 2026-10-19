/* SPDX-License-Identifier: Apache-2.0 */

/* fuzz_port.c - the seams the host port leaves to a suite, for the fuzzers. */
#include <string.h>

#include "dfu_crc.h"
#include "fuzz_port.h"
#include "tinyfota/tinyfota_port.h"
#include "tinyfota_host.h"
#include "tinyfota_rx.h"

const uint8_t tinyfota_pubkey[65] = {0x04};

int tinyfota_ecdsa_p256_verify(const uint8_t pub[65], const uint8_t *msg, size_t msg_len,
			       const uint8_t sig[64])
{
	(void)pub;
	(void)msg;
	(void)msg_len;
	(void)sig;
	return 0;
}

void tinyfota_log(int level, const char *fmt, ...)
{
	(void)level;
	(void)fmt;
}

void fuzz_reset(void)
{
	tinyfota_flash_host_reset();
	tinyfota_osal_host_reset();
	tinyfota_rx_reset_all();
	tinyfota_window_open(300000);
}

void fuzz_fix_header(uint8_t hdr[TINYFOTA_HDR_LEN], unsigned fix, const uint8_t *patch,
		     size_t patch_len)
{
	if (fix & 1u) {
		tinyfota_put_le32(TINYFOTA_MAGIC, &hdr[0]);
	}
	if (fix & 2u) {
		hdr[4] = (uint8_t)TINYFOTA_ABI_VERSION;
		hdr[5] = 0;
	}
	if (fix & 4u) {
		tinyfota_put_le32((uint32_t)patch_len, &hdr[8]);
		tinyfota_put_le32(tinyfota_crc32(patch, patch_len), &hdr[16]);
	}
	if (fix & 8u) {
		tinyfota_put_le32(tinyfota_crc32(hdr, TINYFOTA_HDR_CRC_LEN), &hdr[28]);
	}
}
