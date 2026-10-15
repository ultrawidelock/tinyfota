/* psafake: minimal <psa/crypto.h> recording double (see ../psafake.h — the
 * fake does NO crypto; statuses are knobs, outputs are deterministic filler).
 * Macro values are arbitrary-but-distinct; the suites assert the code under
 * test hands exactly these spellings through, nothing more. */
#ifndef PSAFAKE_PSA_CRYPTO_H
#define PSAFAKE_PSA_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "psafake.h"

typedef int32_t psa_status_t;
typedef uint32_t psa_key_id_t;
typedef uint32_t psa_algorithm_t;
typedef uint32_t psa_key_type_t;
typedef uint32_t psa_key_usage_t;

#define PSA_SUCCESS       ((psa_status_t)0)
#define PSA_ERROR_GENERIC ((psa_status_t)-132)
#define PSA_ERROR_BUFFER_TOO_SMALL ((psa_status_t)-138)
#define PSA_KEY_ID_NULL   ((psa_key_id_t)0)

#define PSA_KEY_USAGE_ENCRYPT        ((psa_key_usage_t)0x00000100u)
#define PSA_KEY_USAGE_DECRYPT        ((psa_key_usage_t)0x00000200u)
#define PSA_KEY_USAGE_SIGN_MESSAGE   ((psa_key_usage_t)0x00000400u)
#define PSA_KEY_USAGE_VERIFY_MESSAGE ((psa_key_usage_t)0x00000800u)
#define PSA_KEY_USAGE_SIGN_HASH      ((psa_key_usage_t)0x00001000u)
#define PSA_KEY_USAGE_VERIFY_HASH    ((psa_key_usage_t)0x00002000u)
#define PSA_KEY_USAGE_DERIVE         ((psa_key_usage_t)0x00004000u)
#define PSA_KEY_USAGE_EXPORT         ((psa_key_usage_t)0x00000001u)

#define PSA_ECC_FAMILY_SECP_R1            0x12u
#define PSA_KEY_TYPE_ECC_KEY_PAIR(f)      ((psa_key_type_t)(0x7100u | (f)))
#define PSA_KEY_TYPE_ECC_PUBLIC_KEY(f)    ((psa_key_type_t)(0x4100u | (f)))

#define PSA_ALG_SHA_256        ((psa_algorithm_t)0x02000009u)
#define PSA_ALG_ECDSA(hash)    ((psa_algorithm_t)(0x06000600u | ((hash) & 0xffu)))

typedef struct {
	psa_key_usage_t usage;
	psa_algorithm_t alg;
	psa_key_type_t type;
	size_t bits;
} psa_key_attributes_t;

#define PSA_KEY_ATTRIBUTES_INIT {0u, 0u, 0u, 0u}


static inline void psa_set_key_usage_flags(psa_key_attributes_t *a, psa_key_usage_t usage)
{
	a->usage = usage;
}
static inline void psa_set_key_algorithm(psa_key_attributes_t *a, psa_algorithm_t alg)
{
	a->alg = alg;
}
static inline void psa_set_key_type(psa_key_attributes_t *a, psa_key_type_t type)
{
	a->type = type;
}
static inline void psa_set_key_bits(psa_key_attributes_t *a, size_t bits)
{
	a->bits = bits;
}

psa_status_t psa_crypto_init(void);
psa_status_t psa_import_key(const psa_key_attributes_t *attributes, const uint8_t *data,
			    size_t data_length, psa_key_id_t *key);
psa_status_t psa_destroy_key(psa_key_id_t key);
psa_status_t psa_verify_message(psa_key_id_t key, psa_algorithm_t alg, const uint8_t *input,
				size_t input_length, const uint8_t *signature,
				size_t signature_length);
psa_status_t psa_hash_compute(psa_algorithm_t alg, const uint8_t *input, size_t input_length,
			      uint8_t *hash, size_t hash_size, size_t *hash_length);
psa_status_t psa_verify_hash(psa_key_id_t key, psa_algorithm_t alg, const uint8_t *hash,
			     size_t hash_length, const uint8_t *signature,
			     size_t signature_length);

#endif /* PSAFAKE_PSA_CRYPTO_H */
