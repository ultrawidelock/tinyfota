/* psafake — test-side control/inspection API for the fake PSA Crypto surface
 * under tests/host/psafake/psa/.
 *
 * THE FAKE DOES NO CRYPTO. Every psa_* entry point records its arguments
 * (key bits, algorithm, usage, lengths); every return status is a knob.
 * Trimmed to the surface the signature seam (ports/zephyr/crypto_psa.c)
 * calls. The suites built on it prove
 * only that the code under test plumbs the right parameters into the API and
 * takes the right branch on each failure — never that any ciphertext is
 * correct. */
#ifndef TINYFOTA_PSAFAKE_H
#define TINYFOTA_PSAFAKE_H

#include <stddef.h>
#include <stdint.h>

#define PSAFAKE_MAX_KEY 65u

struct psafake_state {
	/* knobs: injected return statuses (0 = PSA_SUCCESS) */
	int32_t init_ret;
	int32_t import_ret;
	int32_t verify_ret;
	int32_t hash_ret;

	/* knobs: output-length overrides (-1 = natural length) */
	long hash_olen;

	/* recorded: last key attributes at import */
	uint32_t attr_usage;
	uint32_t attr_alg;
	uint32_t attr_type;
	size_t attr_bits;

	/* recorded: imported key material + call tallies */
	uint8_t key[PSAFAKE_MAX_KEY];
	size_t key_len;
	unsigned init_calls, import_calls;
	unsigned verify_calls, destroy_calls;
	/* ---- replayed ECDSA verification -------------------------------------
	 *
	 * There is no P-256 in this host build, so a signature check can only
	 * be made to mean something by replaying one a real curve produced --
	 * the arrangement spakefake.c already uses for SPAKE2+, with the vector
	 * generated offline by tests/host/gen_dfu_vector.py.
	 *
	 * When verify_replay is set, psa_verify_message() accepts ONLY the
	 * recorded (message, signature) pair and refuses anything else. That
	 * refusal is the check: it proves the caller handed PSA exactly the
	 * bytes a real ECDSA-P256 signed, so a tampered header is rejected for
	 * a real reason rather than because a knob said so.
	 *
	 * psa_verify_hash() holds a caller that hashes first to the same pair:
	 * the message it last gave psa_hash_compute() must be the recorded one,
	 * and the digest it hands over must be the one that call returned. */
	int verify_replay;
	uint8_t replay_msg[64];
	size_t replay_msg_len;
	uint8_t replay_sig[64];
	unsigned verify_rejects;

	uint32_t last_destroyed; /* key id handed to psa_destroy_key */
	uint32_t last_alg;       /* alg argument of the last operation call */
	size_t last_msg_len, last_sig_len;

	/* psa_hash_compute(): the message (its first 64 bytes), its length and
	 * the digest returned for it; psa_verify_hash(): the digest length. */
	unsigned hash_calls;
	uint8_t hashed_msg[64];
	size_t hashed_len;
	uint8_t hashed_digest[32];
	size_t last_hash_len;

};

extern struct psafake_state psafake;

/** @brief Zero all recordings, restore every knob to success/natural length. */
void psafake_reset(void);

#endif /* TINYFOTA_PSAFAKE_H */
