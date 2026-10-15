/* smpfake: <zcbor_encode.h>. Every put appends one typed item to the recorded
 * output and returns true, unless the encode_fail_in knob says otherwise --
 * which is how the EMSGSIZE branches are reached. */
#ifndef SMPFAKE_ZCBOR_ENCODE_H
#define SMPFAKE_ZCBOR_ENCODE_H

#include <zcbor_common.h>

bool zcbor_map_start_encode(zcbor_state_t *state, size_t max_num);
bool zcbor_map_end_encode(zcbor_state_t *state, size_t max_num);
bool zcbor_list_start_encode(zcbor_state_t *state, size_t max_num);
bool zcbor_list_end_encode(zcbor_state_t *state, size_t max_num);

bool zcbor_tstr_put_term_impl(zcbor_state_t *state, const char *str, size_t maxlen);
bool zcbor_bstr_encode(zcbor_state_t *state, const struct zcbor_string *input);
bool zcbor_uint32_put(zcbor_state_t *state, uint32_t value);
bool zcbor_int32_put(zcbor_state_t *state, int32_t value);
bool zcbor_size_put(zcbor_state_t *state, size_t value);
bool zcbor_bool_put(zcbor_state_t *state, bool value);

/* Real zcbor spells these as macros over a string literal, and dfu_smp_img.c
 * relies on that: the literal's length is known at the call site. */
#define zcbor_tstr_put_lit(state, str) zcbor_tstr_put_term_impl((state), (str), sizeof(str))
#define zcbor_tstr_put_term(state, str, maxlen)                                                    \
	zcbor_tstr_put_term_impl((state), (str), (maxlen))

#endif /* SMPFAKE_ZCBOR_ENCODE_H */
