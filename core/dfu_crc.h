/* SPDX-License-Identifier: ISC */

/*
 * dfu_crc.h - CRC-32 (IEEE 802.3, reflected) for the delta-update halves.
 *
 * Chainable exactly as Python's zlib.crc32(data, previous) is -- seed 0, feed
 * runs in order -- which is what lets the host compute the same value without
 * either side reimplementing the other's polynomial. Its own tiny
 * implementation (not Zephyr's crc32_ieee) so both DFU halves compile from
 * one source on every port; the algorithm is identical.
 */
#ifndef TINYFOTA_DFU_CRC_H
#define TINYFOTA_DFU_CRC_H

#include <stddef.h>
#include <stdint.h>

/** @brief Continue a CRC-32 over @p len bytes; seed @p crc with 0 to start. */
uint32_t tinyfota_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/** @brief CRC-32 of one whole buffer. */
static inline uint32_t tinyfota_crc32(const uint8_t *data, size_t len)
{
	return tinyfota_crc32_update(0, data, len);
}

#endif /* TINYFOTA_DFU_CRC_H */
