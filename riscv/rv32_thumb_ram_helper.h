/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_THUMB_RAM_HELPER_H
#define GPSP_RV32_THUMB_RAM_HELPER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Generated-code ABI: A1 is the guest address. Call with
 * jalr RV_TMP0,RV_TMP1,0; the caller charges the fetch after the access.
 * RA exits the block. The _flow entries preserve a retry continuation
 * when the cycle budget is exhausted. */
void gpsp_rv32_ram_load_u8(void);
void gpsp_rv32_ram_load_u16(void);
void gpsp_rv32_ram_load_u32(void);
void gpsp_rv32_ram_load_s8(void);
void gpsp_rv32_ram_load_s16(void);
void gpsp_rv32_ram_load_u8_flow(void);
void gpsp_rv32_ram_load_u16_flow(void);
void gpsp_rv32_ram_load_u32_flow(void);
void gpsp_rv32_ram_load_s8_flow(void);
void gpsp_rv32_ram_load_s16_flow(void);

#ifdef __cplusplus
}
#endif

#endif
