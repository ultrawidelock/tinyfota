/* smpfake: <zcbor_decode.h>. The decoders are only ever reached through
 * zcbor_map_decode_bulk(), which serves the table smpfake_request() installed,
 * so these exist to name the per-type decoder in a key/value entry. */
#ifndef SMPFAKE_ZCBOR_DECODE_H
#define SMPFAKE_ZCBOR_DECODE_H

#include <zcbor_common.h>

bool zcbor_bstr_decode(zcbor_state_t *state, struct zcbor_string *result);
bool zcbor_bool_decode(zcbor_state_t *state, bool *result);
bool zcbor_uint32_decode(zcbor_state_t *state, uint32_t *result);
bool zcbor_size_decode(zcbor_state_t *state, size_t *result);

#endif /* SMPFAKE_ZCBOR_DECODE_H */
