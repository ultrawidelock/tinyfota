/* SPDX-License-Identifier: ISC */

/* dfu_crc.c - see dfu_crc.h. Bitwise, no table: this runs over a few hundred
 * KB once per update, not on a hot path, and the 1 KB table would live in the
 * bootloader's flash. */
#include "dfu_crc.h"

uint32_t tinyfota_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
	crc = ~crc;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
		}
	}
	return ~crc;
}
