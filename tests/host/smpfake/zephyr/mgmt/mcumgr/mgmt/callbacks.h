/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/* smpfake: <zephyr/mgmt/mcumgr/mgmt/callbacks.h> — the event hook a handler
 * uses to veto a command. tinyfota registers one on os_mgmt reset. */
#ifndef SMPFAKE_ZEPHYR_MGMT_MCUMGR_CALLBACKS_H
#define SMPFAKE_ZEPHYR_MGMT_MCUMGR_CALLBACKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** What a callback tells mcumgr to do with the command it intercepted. */
enum mgmt_cb_return {
	MGMT_CB_OK,
	MGMT_CB_ERROR_RC,
	MGMT_CB_ERROR_ERR,
};

/* One event id is enough here; a second distinct value lets a suite prove the
 * hook ignores everything it was not registered for. */
#define MGMT_EVT_OP_OS_MGMT_RESET  0x00000040u
#define MGMT_EVT_OP_IMG_MGMT_DFU_STARTED 0x00000200u

typedef enum mgmt_cb_return (*mgmt_cb)(uint32_t event, enum mgmt_cb_return prev_status, int32_t *rc,
				       uint16_t *group, bool *abort_more, void *data,
				       size_t data_size);

/** A registered event hook. */
struct mgmt_callback {
	mgmt_cb callback;
	uint32_t event_id;
};

void mgmt_callback_register(struct mgmt_callback *callback);

#endif /* SMPFAKE_ZEPHYR_MGMT_MCUMGR_CALLBACKS_H */
