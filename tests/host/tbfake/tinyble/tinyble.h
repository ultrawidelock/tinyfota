/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/* tbfake: the part of tinyble's <tinyble/tinyble.h> that ports/zephyr/dfu_tinyble.c
 * reaches. A test double written from the interface tinyfota was handed, not a
 * copy of tinyble's header: the error values, tb_notify() and tb_mtu(). */
#ifndef TBFAKE_TINYBLE_H
#define TBFAKE_TINYBLE_H

#include <stdint.h>

/* Zephyr's errno numbers, as tinyble defines its returns. */
#define TB_EAGAIN   (-11)
#define TB_EBUSY    (-16)
#define TB_EINVAL   (-22)
#define TB_ENOTCONN (-128)

int tb_notify(uint16_t handle, const uint8_t *data, uint16_t len);
uint16_t tb_mtu(void);

#endif /* TBFAKE_TINYBLE_H */
