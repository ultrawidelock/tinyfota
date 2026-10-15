/* smpfake — recording zcbor encoder, table-driven zcbor decoder, and an
 * mcumgr registry that records. See smpfake.h for what this is worth. */

#include "smpfake.h"

#include <string.h>

#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>

struct smpfake_state smpfake;

static struct smpfake_kv request_kv[SMPFAKE_MAX_KEYS];
static size_t request_count;

void smpfake_reset(void)
{
	memset(&smpfake, 0, sizeof(smpfake));
	smpfake.encode_fail_in = -1;
	smpfake.decode_bulk_ret = 0;
	request_count = 0;
}

void smpfake_request(const struct smpfake_kv *kv, size_t count)
{
	if (count > SMPFAKE_MAX_KEYS) {
		count = SMPFAKE_MAX_KEYS;
	}
	if (kv != NULL && count > 0U) {
		memcpy(request_kv, kv, count * sizeof(*kv));
	}
	request_count = (kv != NULL) ? count : 0U;
}

int smpfake_find(const char *key)
{
	for (size_t i = 0; i < smpfake.item_count; i++) {
		if (smpfake.items[i].kind == SMPFAKE_TSTR &&
		    strcmp(smpfake.items[i].text, key) == 0) {
			return (int)i;
		}
	}
	return -1;
}

const struct smpfake_item *smpfake_item(int index)
{
	if (index < 0 || (size_t)index >= smpfake.item_count) {
		return NULL;
	}
	return &smpfake.items[index];
}

/* ---- encoder -------------------------------------------------------------- */

/* Append one item, unless the knob has this put failing. Returning false is
 * what walks dfu_smp_img.c into its EMSGSIZE branches. */
static struct smpfake_item *append(enum smpfake_kind kind)
{
	struct smpfake_item *item;

	if (smpfake.encode_fail_in >= 0) {
		if (smpfake.encode_fail_in == 0) {
			return NULL;
		}
		smpfake.encode_fail_in--;
	}
	if (smpfake.item_count >= SMPFAKE_MAX_ITEMS) {
		smpfake.overflowed = true;
		return NULL;
	}
	item = &smpfake.items[smpfake.item_count++];
	memset(item, 0, sizeof(*item));
	item->kind = kind;
	return item;
}

bool zcbor_map_start_encode(zcbor_state_t *state, size_t max_num)
{
	struct smpfake_item *item = append(SMPFAKE_MAP_START);

	(void)state;
	if (item == NULL) {
		return false;
	}
	item->value = (int64_t)max_num;
	return true;
}

bool zcbor_map_end_encode(zcbor_state_t *state, size_t max_num)
{
	struct smpfake_item *item = append(SMPFAKE_MAP_END);

	(void)state;
	if (item == NULL) {
		return false;
	}
	item->value = (int64_t)max_num;
	return true;
}

bool zcbor_list_start_encode(zcbor_state_t *state, size_t max_num)
{
	struct smpfake_item *item = append(SMPFAKE_LIST_START);

	(void)state;
	if (item == NULL) {
		return false;
	}
	item->value = (int64_t)max_num;
	return true;
}

bool zcbor_list_end_encode(zcbor_state_t *state, size_t max_num)
{
	struct smpfake_item *item = append(SMPFAKE_LIST_END);

	(void)state;
	if (item == NULL) {
		return false;
	}
	item->value = (int64_t)max_num;
	return true;
}

/* strnlen is POSIX, not ISO C, and glibc hides it under -std=c11. One call site,
 * so a local bounded length beats a feature-test macro that would also change
 * which BSD extensions Darwin exposes to this file. */
static size_t bounded_len(const char *s, size_t maxlen)
{
	size_t n = 0;

	while (n < maxlen && s[n] != '\0') {
		n++;
	}
	return n;
}

bool zcbor_tstr_put_term_impl(zcbor_state_t *state, const char *str, size_t maxlen)
{
	struct smpfake_item *item = append(SMPFAKE_TSTR);
	size_t n;

	(void)state;
	if (item == NULL) {
		return false;
	}
	n = bounded_len(str, maxlen);
	if (n >= SMPFAKE_MAX_TEXT) {
		n = SMPFAKE_MAX_TEXT - 1u;
	}
	memcpy(item->text, str, n);
	item->text[n] = '\0';
	item->len = n;
	return true;
}

bool zcbor_bstr_encode(zcbor_state_t *state, const struct zcbor_string *input)
{
	struct smpfake_item *item = append(SMPFAKE_BSTR);
	size_t n;

	(void)state;
	if (item == NULL) {
		return false;
	}
	n = input->len;
	if (n > SMPFAKE_MAX_BYTES) {
		n = SMPFAKE_MAX_BYTES;
	}
	if (input->value != NULL) {
		memcpy(item->bytes, input->value, n);
	}
	item->len = n;
	return true;
}

bool zcbor_uint32_put(zcbor_state_t *state, uint32_t value)
{
	struct smpfake_item *item = append(SMPFAKE_UINT);

	(void)state;
	if (item == NULL) {
		return false;
	}
	item->value = (int64_t)value;
	return true;
}

bool zcbor_int32_put(zcbor_state_t *state, int32_t value)
{
	struct smpfake_item *item = append(SMPFAKE_INT);

	(void)state;
	if (item == NULL) {
		return false;
	}
	item->value = value;
	return true;
}

bool zcbor_size_put(zcbor_state_t *state, size_t value)
{
	struct smpfake_item *item = append(SMPFAKE_UINT);

	(void)state;
	if (item == NULL) {
		return false;
	}
	item->value = (int64_t)value;
	return true;
}

bool zcbor_bool_put(zcbor_state_t *state, bool value)
{
	struct smpfake_item *item = append(SMPFAKE_BOOL);

	(void)state;
	if (item == NULL) {
		return false;
	}
	item->value = value ? 1 : 0;
	return true;
}

/* ---- decoder --------------------------------------------------------------
 *
 * Never reached individually: zcbor_map_decode_bulk() writes through the
 * value pointers itself. They exist so a key table can name a decoder, which
 * is how the handler under test is written.
 */

bool zcbor_bstr_decode(zcbor_state_t *state, struct zcbor_string *result)
{
	(void)state;
	(void)result;
	return true;
}

bool zcbor_bool_decode(zcbor_state_t *state, bool *result)
{
	(void)state;
	(void)result;
	return true;
}

bool zcbor_uint32_decode(zcbor_state_t *state, uint32_t *result)
{
	(void)state;
	(void)result;
	return true;
}

bool zcbor_size_decode(zcbor_state_t *state, size_t *result)
{
	(void)state;
	(void)result;
	return true;
}

/* Match the handler's key table against the fields the suite installed, and
 * write each match through the table's own value pointer using the decoder it
 * named — so a handler that wires the wrong decoder to a key is caught. */
int zcbor_map_decode_bulk(zcbor_state_t *state, struct zcbor_map_decode_key_val *map,
			  size_t map_size, size_t *matched)
{
	size_t found = 0;

	(void)state;
	smpfake.decode_bulk_calls++;

	if (smpfake.decode_bulk_ret != 0) {
		*matched = 0;
		return smpfake.decode_bulk_ret;
	}

	for (size_t i = 0; i < map_size; i++) {
		for (size_t k = 0; k < request_count; k++) {
			const struct smpfake_kv *kv = &request_kv[k];
			size_t key_len = strlen(kv->key);

			if (key_len != map[i].key.len ||
			    memcmp(kv->key, map[i].key.value, key_len) != 0) {
				continue;
			}
			if (map[i].decoder == (void *)zcbor_uint32_decode &&
			    kv->kind == SMPFAKE_UINT) {
				*(uint32_t *)map[i].value_ptr = (uint32_t)kv->value;
			} else if (map[i].decoder == (void *)zcbor_size_decode &&
				   kv->kind == SMPFAKE_UINT) {
				*(size_t *)map[i].value_ptr = (size_t)kv->value;
			} else if (map[i].decoder == (void *)zcbor_bool_decode &&
				   kv->kind == SMPFAKE_BOOL) {
				*(bool *)map[i].value_ptr = kv->value != 0U;
			} else if (map[i].decoder == (void *)zcbor_bstr_decode &&
				   kv->kind == SMPFAKE_BSTR) {
				struct zcbor_string *out = map[i].value_ptr;

				out->value = kv->bytes;
				out->len = kv->len;
			} else {
				/* The handler named a decoder that does not fit
				 * the field. Leave it unmatched rather than
				 * papering over it. */
				continue;
			}
			map[i].found = true;
			found++;
			break;
		}
	}

	smpfake.decoded_keys = found;
	*matched = found;
	return 0;
}

/* ---- mcumgr registry ------------------------------------------------------ */

void mgmt_register_group(struct mgmt_group *group)
{
	smpfake.register_group_calls++;
	smpfake.registered_group = group;
}

void mgmt_callback_register(struct mgmt_callback *callback)
{
	smpfake.register_callback_calls++;
	smpfake.registered_callback = callback;
}
