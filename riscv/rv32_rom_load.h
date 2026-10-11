/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef GPSP_RV32_ROM_LOAD_H
#define GPSP_RV32_ROM_LOAD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Generated-code ABI: a0 is the C callback, a1 the guest address, and ra
 * the continuation. A hit returns the value in a0, zero in a1, and restores
 * ra to the block exit. Other accesses tail-call the C bridge. */
void gpsp_rv32_rom_load_u8(void);
void gpsp_rv32_rom_load_u16(void);
void gpsp_rv32_rom_load_u32(void);
void gpsp_rv32_rom_load_s8(void);
void gpsp_rv32_rom_load_s16(void);

/* Probe ABI: called through RV_TMP0 with a1 the guest address. A hit loads
 * a0, charges the wait and returns to RV_TMP0; a miss returns to
 * RV_TMP0 + 4. Uses a2 and RV_TMP1. */
void gpsp_rv32_rom_probe_u8(void);
void gpsp_rv32_rom_probe_u16(void);
void gpsp_rv32_rom_probe_u32(void);
void gpsp_rv32_rom_probe_s8(void);
void gpsp_rv32_rom_probe_s16(void);

#ifdef __cplusplus
}
#endif

#endif
