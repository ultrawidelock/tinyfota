/* smpfake: <zcbor_common.h> — the state handle and byte-string type.
 * See ../smpfake.h: this records puts, it does not produce CBOR. */
#ifndef SMPFAKE_ZCBOR_COMMON_H
#define SMPFAKE_ZCBOR_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** zcbor's opaque coding state. Nothing in it matters to the recorder. */
typedef struct {
	int unused;
} zcbor_state_t;

/** zcbor's counted string, used for both text and byte strings. */
struct zcbor_string {
	const uint8_t *value;
	size_t len;
};

#endif /* SMPFAKE_ZCBOR_COMMON_H */
