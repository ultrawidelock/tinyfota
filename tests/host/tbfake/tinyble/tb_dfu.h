/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/* tbfake: <tinyble/tb_dfu.h>, the two value handles and the three hooks
 * dfu_tinyble.c provides, as the contract with tinyble states them. */
#ifndef TBFAKE_TB_DFU_H
#define TBFAKE_TB_DFU_H

#include <tinyble/tinyble.h>

#define TB_DFU_H_SMP    0x0014u
#define TB_DFU_H_NATIVE 0x0018u

uint8_t tb_dfu_on_write(uint16_t handle, const uint8_t *data, uint16_t len);
void tb_dfu_on_ccc(uint16_t handle, uint16_t value);
void tb_dfu_on_disconnect(uint8_t reason);

#endif /* TBFAKE_TB_DFU_H */
