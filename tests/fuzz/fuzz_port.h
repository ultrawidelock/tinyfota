/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * fuzz_port.h - what the libFuzzer targets share. The flash and the clock are
 * the host port the unit tests run on (tests/host/port/): RAM partitions with
 * the nRF word and page rules, so an unaligned or out-of-range access comes
 * back as an error exactly as it would on target, and a virtual clock. The
 * signature seam accepts everything (fuzz_port.c): the receiver's P-256 check
 * is not what is under test, and refusing would stop every input at the
 * first gate.
 */
#ifndef TINYFOTA_FUZZ_PORT_H
#define TINYFOTA_FUZZ_PORT_H

#include <stddef.h>
#include <stdint.h>

#include "tinyfota.h"

/** Fresh board: both partitions erased, receiver idle, update window open. */
void fuzz_reset(void);

/**
 * Make a 32 B header pass the checks a fuzzer cannot satisfy by mutation:
 * magic and ABI version as @p fix says (bit 0, bit 1), the patch CRC over
 * @p patch (bit 2), and the header CRC last (bit 3). Bits clear leave the
 * fuzzer's bytes alone, so both sides of every check stay reachable.
 */
void fuzz_fix_header(uint8_t hdr[TINYFOTA_HDR_LEN], unsigned fix, const uint8_t *patch,
		     size_t patch_len);

#endif /* TINYFOTA_FUZZ_PORT_H */
