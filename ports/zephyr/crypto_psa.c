/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * crypto_psa.c - the signature seam in tinyfota_port.h, on Arm PSA Crypto.
 * Plain PSA with no Zephyr include, so the host suite compiles this same file
 * against its recording PSA double (tests/host/psafake/) and checks the calls.
 */
#include <stddef.h>
#include <stdint.h>

#include "psa/crypto.h"

#include "tinyfota/tinyfota_port.h"

#define TINYFOTA_P256_POINT 65u
#define TINYFOTA_P256_SIG   64u

// Verify an ECDSA-P256 signature against an already computed SHA-256 digest and public key, via
// PSA Crypto. Returns 0 if the signature verifies, -1 if public key import fails or verification
// fails.
static int verify_hash(const uint8_t pub[TINYFOTA_P256_POINT], const uint8_t hash[32],
		       const uint8_t sig[TINYFOTA_P256_SIG])
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t k = 0;
	int rc = -1;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	if (psa_import_key(&attr, pub, TINYFOTA_P256_POINT, &k) != PSA_SUCCESS) {
		return -1;
	}
	if (psa_verify_hash(k, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, 32u, sig,
			    TINYFOTA_P256_SIG) == PSA_SUCCESS) {
		rc = 0;
	}
	psa_destroy_key(k);
	return rc;
}

// Verify an ECDSA-P256/SHA-256 signature against a message and public key, via PSA Crypto.
// Returns 0 if the signature verifies, -1 if init, hashing, public key import or verification
// fails.
//
// Hashes here and takes the digest path below, so an image carries one PSA verify entry. With
// psa_verify_message kept next to psa_verify_hash the nRF52833 lock images grew by about 230
// bytes (measured in ultrawidelock).
//
// psa_crypto_init() is idempotent and is called on every verify, so the receiver needs no init
// call of its own and fails closed when the provider cannot start.
int tinyfota_ecdsa_p256_verify(const uint8_t pub[TINYFOTA_P256_POINT], const uint8_t *msg,
			       size_t msg_len, const uint8_t sig[TINYFOTA_P256_SIG])
{
	uint8_t hash[32];
	size_t hlen = 0;

	if (psa_crypto_init() != PSA_SUCCESS) {
		return -1;
	}
	if (psa_hash_compute(PSA_ALG_SHA_256, msg, msg_len, hash, sizeof(hash), &hlen) !=
		    PSA_SUCCESS ||
	    hlen != sizeof(hash)) {
		return -1;
	}
	return verify_hash(pub, hash, sig);
}
