/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief The SMP image group and the reset gate, once, for every SMP server.
 *
 * Transport-neutral. Two front ends call it: ports/zephyr/dfu_smp_img.c
 * registers it with mcumgr (Zephyr Bluetooth host builds), and
 * ports/zephyr/dfu_tinyble.c serves it from tinyfota's own SMP server on
 * tinyble, which has no mcumgr. Each hands over a zcbor decoder positioned on
 * the request's map and an encoder positioned inside the response's map, and
 * gets back an mcumgr status.
 */

#ifndef TINYFOTA_SMP_GROUP_H_
#define TINYFOTA_SMP_GROUP_H_

#include <stdbool.h>
#include <stddef.h>

#include <zcbor_common.h>

/* mcumgr's MGMT_ERR_* (zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h), which are the
 * values on the wire. Spelled out so a build without mcumgr uses the same
 * numbers; dfu_smp_img.c checks them against mcumgr's at build time. */
enum tinyfota_smp_rc {
	TINYFOTA_SMP_RC_OK = 0,
	TINYFOTA_SMP_RC_EUNKNOWN = 1,
	TINYFOTA_SMP_RC_ENOMEM = 2,
	TINYFOTA_SMP_RC_EINVAL = 3,
	TINYFOTA_SMP_RC_EBADSTATE = 6,
	TINYFOTA_SMP_RC_EMSGSIZE = 7,
	TINYFOTA_SMP_RC_ENOTSUP = 8,
	TINYFOTA_SMP_RC_EACCESSDENIED = 11,
	TINYFOTA_SMP_RC_TOO_NEW = 13,
};

/* MGMT_GROUP_ID_IMAGE and its command ids. Spelled out rather than included
 * from img_mgmt.h, which is not compiled in this configuration. */
#define TINYFOTA_SMP_GRP_IMG 1

#define TINYFOTA_SMP_IMG_ID_STATE  0
#define TINYFOTA_SMP_IMG_ID_UPLOAD 1
#define TINYFOTA_SMP_IMG_ID_ERASE  5

/** Image state, read: the one slot this board has. */
int tinyfota_smp_img_state_read(zcbor_state_t *zse);

/** Image state, write: accepted, changes nothing, answers with the list. */
int tinyfota_smp_img_state_write(zcbor_state_t *zsd, zcbor_state_t *zse);

/** Image upload: one chunk into tinyfota_rx_upload(). */
int tinyfota_smp_img_upload(zcbor_state_t *zsd, zcbor_state_t *zse);

/** Image erase: drop whatever is staged, behind the window. */
int tinyfota_smp_img_erase(void);

/**
 * May os group command 5 reboot the board? Only with an update window open
 * or a verified patch staged. Logs the refusal.
 */
bool tinyfota_smp_reset_allowed(void);

/* ---- the request map ------------------------------------------------------ */

/** What a key's value must be. Each names one zcbor decoder. */
enum tinyfota_smp_type {
	TINYFOTA_SMP_UINT32, /**< zcbor_uint32_decode into uint32_t */
	TINYFOTA_SMP_SIZE,   /**< zcbor_size_decode into size_t */
	TINYFOTA_SMP_BOOL,   /**< zcbor_bool_decode into bool */
	TINYFOTA_SMP_BSTR,   /**< zcbor_bstr_decode into struct zcbor_string */
	TINYFOTA_SMP_TSTR,   /**< zcbor_tstr_decode into struct zcbor_string */
};

/** One key a handler wants out of the request map. */
struct tinyfota_smp_key {
	const char *key;
	size_t key_len;
	enum tinyfota_smp_type type;
	void *value;
	bool found;
};

/** A key table entry; @p k must be a string literal. */
#define TINYFOTA_SMP_KEY(k, t, v) {(k), sizeof(k) - 1, (t), (v), false}

/**
 * Decode one request map into @p keys.
 *
 * A key that is not in the table is skipped when its value is a scalar or a
 * string. A list, map or tag under such a key is refused instead: zcbor skips
 * those by recursing, once per nesting level, and a request a few hundred
 * bytes long would then decide how deep the stack goes. No SMP request this
 * serves carries one.
 *
 * @param[out] matched  how many table keys were found
 * @retval 0 the map decoded; keys not in the request keep their values
 * @retval negative malformed: not a map, a key that is not a text string, a
 *         value of the wrong type, a key given twice, or a nested value
 */
int tinyfota_smp_map_decode(zcbor_state_t *zsd, struct tinyfota_smp_key *keys, size_t n,
			    size_t *matched);

#endif /* TINYFOTA_SMP_GROUP_H_ */
