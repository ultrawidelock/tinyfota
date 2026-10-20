/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/* psafake implementations — recording doubles only (see psafake.h: the fake
 * does NO crypto). Every status/olen is a knob. Trimmed to the signature seam's
 * calls. */
#include <string.h>

#include "psafake.h"
#include <psa/crypto.h>

struct psafake_state psafake;

void psafake_reset(void)
{
	memset(&psafake, 0, sizeof(psafake));
	psafake.hash_olen = -1;
}

psa_status_t psa_crypto_init(void)
{
	psafake.init_calls++;
	return psafake.init_ret;
}

psa_status_t psa_import_key(const psa_key_attributes_t *attributes, const uint8_t *data,
			    size_t data_length, psa_key_id_t *key)
{
	psafake.import_calls++;
	psafake.attr_usage = attributes->usage;
	psafake.attr_alg = attributes->alg;
	psafake.attr_type = attributes->type;
	psafake.attr_bits = attributes->bits;
	psafake.key_len = data_length <= PSAFAKE_MAX_KEY ? data_length : PSAFAKE_MAX_KEY;
	memcpy(psafake.key, data, psafake.key_len);
	if (psafake.import_ret != PSA_SUCCESS) {
		return psafake.import_ret;
	}
	*key = (psa_key_id_t)(0x1000u + psafake.import_calls);
	return PSA_SUCCESS;
}

psa_status_t psa_destroy_key(psa_key_id_t key)
{
	psafake.destroy_calls++;
	psafake.last_destroyed = key;
	return PSA_SUCCESS;
}

/** Serve *olen: natural value unless the knob overrides it. */
static size_t olen_of(long knob, size_t natural)
{
	return knob >= 0 ? (size_t)knob : natural;
}

psa_status_t psa_verify_message(psa_key_id_t key, psa_algorithm_t alg, const uint8_t *input,
				size_t input_length, const uint8_t *signature,
				size_t signature_length)
{
	(void)key;
	psafake.verify_calls++;
	psafake.last_alg = alg;
	psafake.last_msg_len = input_length;
	psafake.last_sig_len = signature_length;
	if (psafake.verify_replay) {
		/* Exactly the recorded pair, or nothing. See psafake.h. */
		if (input_length != psafake.replay_msg_len || signature_length != 64u ||
		    memcmp(input, psafake.replay_msg, input_length) != 0 ||
		    memcmp(signature, psafake.replay_sig, 64) != 0) {
			psafake.verify_rejects++;
			return PSA_ERROR_GENERIC;
		}
		return PSA_SUCCESS;
	}
	return psafake.verify_ret;
}

/* There is no SHA-256 in this host build: the digest is a fold of the message
 * and its length, enough to tell two messages apart. What was hashed is
 * recorded, so a replayed verification can hold the caller to it. */
psa_status_t psa_hash_compute(psa_algorithm_t alg, const uint8_t *input, size_t input_length,
			      uint8_t *hash, size_t hash_size, size_t *hash_length)
{
	size_t keep = input_length < sizeof(psafake.hashed_msg) ? input_length
								: sizeof(psafake.hashed_msg);

	psafake.hash_calls++;
	psafake.last_alg = alg;
	psafake.last_msg_len = input_length;
	psafake.hashed_len = input_length;
	memset(psafake.hashed_msg, 0, sizeof(psafake.hashed_msg));
	memcpy(psafake.hashed_msg, input, keep);
	memset(psafake.hashed_digest, 0, sizeof(psafake.hashed_digest));
	for (size_t i = 0; i < input_length; i++) {
		uint8_t *d = &psafake.hashed_digest[i % sizeof(psafake.hashed_digest)];

		*d = (uint8_t)(*d * 31u + input[i] + i);
	}
	psafake.hashed_digest[31] ^= (uint8_t)input_length;
	if (hash_size < sizeof(psafake.hashed_digest)) {
		return PSA_ERROR_GENERIC;
	}
	memcpy(hash, psafake.hashed_digest, sizeof(psafake.hashed_digest));
	*hash_length = olen_of(psafake.hash_olen, sizeof(psafake.hashed_digest));
	return psafake.hash_ret;
}

psa_status_t psa_verify_hash(psa_key_id_t key, psa_algorithm_t alg, const uint8_t *hash,
			     size_t hash_length, const uint8_t *signature,
			     size_t signature_length)
{
	(void)key;
	psafake.verify_calls++;
	psafake.last_alg = alg;
	psafake.last_hash_len = hash_length;
	psafake.last_sig_len = signature_length;
	if (psafake.verify_replay) {
		/* Exactly the recorded pair, or nothing: the digest of the recorded
		 * message, as this fake computed it, and its signature. See psafake.h. */
		if (psafake.hashed_len != psafake.replay_msg_len || hash_length != 32u ||
		    signature_length != 64u ||
		    memcmp(psafake.hashed_msg, psafake.replay_msg, psafake.replay_msg_len) != 0 ||
		    memcmp(hash, psafake.hashed_digest, 32) != 0 ||
		    memcmp(signature, psafake.replay_sig, 64) != 0) {
			psafake.verify_rejects++;
			return PSA_ERROR_GENERIC;
		}
		return PSA_SUCCESS;
	}
	return psafake.verify_ret;
}
