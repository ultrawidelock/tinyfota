/* smpfake: <mgmt/mcumgr/util/zcbor_bulk.h>. The bulk map decoder mcumgr
 * handlers use to pull a request apart in one call. */
#ifndef SMPFAKE_ZCBOR_BULK_H
#define SMPFAKE_ZCBOR_BULK_H

#include <stddef.h>

#include <zcbor_common.h>

/** One key and the decoder that should be run for it. */
struct zcbor_map_decode_key_val {
	struct zcbor_string key;
	void *decoder;
	void *value_ptr;
	bool found;
};

/* Real mcumgr builds the key from a string literal; the fake keeps the same
 * shape so the handler's key table is written exactly as it ships. */
#define ZCBOR_MAP_DECODE_KEY_DECODER(k, dec, val)                                                  \
	{                                                                                          \
		{(const uint8_t *)(k), sizeof(k) - 1}, (void *)(dec), (val), false                  \
	}

int zcbor_map_decode_bulk(zcbor_state_t *state, struct zcbor_map_decode_key_val *map,
			  size_t map_size, size_t *matched);

#endif /* SMPFAKE_ZCBOR_BULK_H */
