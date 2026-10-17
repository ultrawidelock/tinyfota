/* SPDX-License-Identifier: ISC */

/**
 * @file
 * @brief Pull an SMP request map apart in one call (smp_group.h).
 *
 * Its own file, apart from the commands in smp_group.c, so the host suite
 * that records the commands' output (tests/host/smpfake) can stand in for it,
 * and the suite over real zcbor (tests/host/test_tinyble.c) can test it.
 */

#include <errno.h>
#include <string.h>

#include <zcbor_common.h>
#include <zcbor_decode.h>

#include "smp_group.h"

/* CBOR major types (RFC 8949 section 3.1) that hold other items. */
#define CBOR_MAJOR_LIST 4u
#define CBOR_MAJOR_MAP  5u
#define CBOR_MAJOR_TAG  6u

static bool decode_value(zcbor_state_t *zsd, struct tinyfota_smp_key *k)
{
	switch (k->type) {
	case TINYFOTA_SMP_UINT32:
		return zcbor_uint32_decode(zsd, k->value);
	case TINYFOTA_SMP_SIZE:
		return zcbor_size_decode(zsd, k->value);
	case TINYFOTA_SMP_BOOL:
		return zcbor_bool_decode(zsd, k->value);
	case TINYFOTA_SMP_BSTR:
		return zcbor_bstr_decode(zsd, k->value);
	case TINYFOTA_SMP_TSTR:
		return zcbor_tstr_decode(zsd, k->value);
	default:
		return false;
	}
}

/* Skip one value, refusing any that holds other items (smp_group.h says why). */
static bool skip_flat(zcbor_state_t *zsd)
{
	uint8_t major;

	if (zsd->payload >= zsd->payload_end) {
		return false;
	}
	major = *zsd->payload >> 5;
	if (major == CBOR_MAJOR_LIST || major == CBOR_MAJOR_MAP || major == CBOR_MAJOR_TAG) {
		return false;
	}
	return zcbor_any_skip(zsd, NULL);
}

int tinyfota_smp_map_decode(zcbor_state_t *zsd, struct tinyfota_smp_key *keys, size_t n,
			    size_t *matched)
{
	struct zcbor_string key;

	*matched = 0;
	if (!zcbor_map_start_decode(zsd)) {
		return -EBADMSG;
	}

	/* A key that is not a text string ends the loop here as well, and the
	 * map end below then fails on the item still in front of it. */
	while (zcbor_tstr_decode(zsd, &key)) {
		struct tinyfota_smp_key *k = NULL;

		for (size_t i = 0; i < n; i++) {
			if (keys[i].key_len == key.len &&
			    memcmp(keys[i].key, key.value, key.len) == 0) {
				k = &keys[i];
				break;
			}
		}
		if (k == NULL) {
			if (!skip_flat(zsd)) {
				return -EBADMSG;
			}
			continue;
		}
		if (k->found || !decode_value(zsd, k)) {
			return -EBADMSG;
		}
		k->found = true;
		(*matched)++;
	}

	return zcbor_map_end_decode(zsd) ? 0 : -EBADMSG;
}
