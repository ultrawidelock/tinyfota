/* smpfake — test-side control/inspection API for the fake zcbor and mcumgr
 * surfaces that ports/zephyr/dfu/dfu_smp_img.c builds against.
 *
 * THE ENCODER RECORDS, IT DOES NOT SERIALISE. dfu_smp_img.c is an adapter:
 * everything it decides is WHICH keys and values it emits, in WHAT order, and
 * which mgmt error it returns. zcbor's byte-level output is upstream's problem
 * and is not under test here, so the fake appends one typed item per put and a
 * suite asserts on that sequence. A wrong key, a wrong value, a missing field
 * or a reordered map all fail; a malformed CBOR byte would not be noticed,
 * because nothing here produces CBOR bytes at all.
 *
 * THE DECODER IS TABLE-DRIVEN for the same reason. zcbor_map_decode_bulk()
 * serves whatever key/value set the suite installed with smpfake_request(),
 * so a request is described in terms of its fields rather than hand-encoded.
 *
 * The handler table is real: MCUMGR_HANDLER_DEFINE() and mgmt_register_group()
 * record what the module registers, and the suite reaches the handlers only
 * through that registration -- so a group id, a handler slot or a read/write
 * pairing that is wrong is caught rather than bypassed.
 */
#ifndef TINYFOTA_SMPFAKE_H
#define TINYFOTA_SMPFAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SMPFAKE_MAX_ITEMS 64
#define SMPFAKE_MAX_TEXT  32
#define SMPFAKE_MAX_BYTES 64
#define SMPFAKE_MAX_KEYS  8

/** What an encoder call appended to the recorded output. */
enum smpfake_kind {
	SMPFAKE_TSTR,
	SMPFAKE_UINT,
	SMPFAKE_INT,
	SMPFAKE_BOOL,
	SMPFAKE_BSTR,
	SMPFAKE_MAP_START,
	SMPFAKE_MAP_END,
	SMPFAKE_LIST_START,
	SMPFAKE_LIST_END,
};

/** One recorded encoder call. */
struct smpfake_item {
	enum smpfake_kind kind;
	char text[SMPFAKE_MAX_TEXT];
	int64_t value;
	uint8_t bytes[SMPFAKE_MAX_BYTES];
	size_t len;
};

/** One field a suite wants zcbor_map_decode_bulk() to produce. */
struct smpfake_kv {
	const char *key;
	enum smpfake_kind kind;
	uint64_t value;      /**< SMPFAKE_UINT / SMPFAKE_BOOL */
	const uint8_t *bytes; /**< SMPFAKE_BSTR */
	size_t len;
};

struct smpfake_state {
	/* recorded encoder output */
	struct smpfake_item items[SMPFAKE_MAX_ITEMS];
	size_t item_count;
	bool overflowed; /**< more puts than SMPFAKE_MAX_ITEMS */

	/* knobs */
	int encode_fail_in;  /**< -1 never; else fail the Nth put onward */
	int decode_bulk_ret; /**< returned by zcbor_map_decode_bulk() */

	/* recorded decoder side */
	unsigned decode_bulk_calls;
	size_t decoded_keys; /**< value written to *decoded */

	/* recorded registration */
	const void *registered_group;
	unsigned register_group_calls;
	const void *registered_callback;
	unsigned register_callback_calls;
};

extern struct smpfake_state smpfake;

/** @brief Zero every recording and restore both knobs. */
void smpfake_reset(void);

/** @brief Install the fields the next zcbor_map_decode_bulk() will produce. */
void smpfake_request(const struct smpfake_kv *kv, size_t count);

/** @brief Index of the first recorded item whose text is @p key, or -1. */
int smpfake_find(const char *key);

/** @brief Item at @p index, or NULL when out of range. */
const struct smpfake_item *smpfake_item(int index);

#endif /* TINYFOTA_SMPFAKE_H */
