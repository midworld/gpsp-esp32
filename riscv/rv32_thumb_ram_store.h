/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_THUMB_RAM_STORE_H
#define GPSP_RV32_THUMB_RAM_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Generated-code ABI: A1 is the guest address and A2 is the value.
 * Call with jalr RV_TMP0,RV_TMP1,0; the caller charges the fetch.
 * RA exits the block. The _flow entries preserve a retry continuation
 * when the cycle budget is exhausted. */
void gpsp_rv32_ram_store_u8(void);
void gpsp_rv32_ram_store_u16(void);
void gpsp_rv32_ram_store_u32(void);
void gpsp_rv32_ram_store_u8_flow(void);
void gpsp_rv32_ram_store_u16_flow(void);
void gpsp_rv32_ram_store_u32_flow(void);

#ifdef __cplusplus
}
#endif

#endif
