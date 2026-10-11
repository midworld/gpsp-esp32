/* Synthetic addresses for code generation; never called or dereferenced.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Keep symbols within 32 bits and in range of host address relocations. */
#if defined(__APPLE__)
#define SYMBOL(name) _##name
#define ADDRESS_BASE 0xc0000000
#else
#define SYMBOL(name) name
#define ADDRESS_BASE 0x40000000
#endif

    .macro address name, value
    .globl \name
    .set \name, ADDRESS_BASE + \value
    .endm

    address SYMBOL(ws_cyc_seq), 0x1000000
    address SYMBOL(gpsp_rv32_call_helper), 0x1000
    address SYMBOL(gpsp_rv32_ram_load_u8), 0x1100
    address SYMBOL(gpsp_rv32_ram_load_u16), 0x1200
    address SYMBOL(gpsp_rv32_ram_load_u32), 0x1300
    address SYMBOL(gpsp_rv32_ram_load_s8), 0x1400
    address SYMBOL(gpsp_rv32_ram_load_s16), 0x1500
    address SYMBOL(gpsp_rv32_ram_store_u8), 0x1600
    address SYMBOL(gpsp_rv32_ram_store_u16), 0x1700
    address SYMBOL(gpsp_rv32_ram_store_u32), 0x1800
    address SYMBOL(gpsp_rv32_rom_load_u8), 0x1900
    address SYMBOL(gpsp_rv32_rom_load_u16), 0x1a00
    address SYMBOL(gpsp_rv32_rom_load_u32), 0x1b00
    address SYMBOL(gpsp_rv32_rom_load_s8), 0x1c00
    address SYMBOL(gpsp_rv32_rom_load_s16), 0x1d00
    address SYMBOL(gpsp_rv32_ram_load_u8_flow), 0x1e00
    address SYMBOL(gpsp_rv32_ram_load_u16_flow), 0x1f00
    address SYMBOL(gpsp_rv32_ram_load_u32_flow), 0x2000
    address SYMBOL(gpsp_rv32_ram_load_s8_flow), 0x2100
    address SYMBOL(gpsp_rv32_ram_load_s16_flow), 0x2200
    address SYMBOL(gpsp_rv32_ram_store_u8_flow), 0x2300
    address SYMBOL(gpsp_rv32_ram_store_u16_flow), 0x2400
    address SYMBOL(gpsp_rv32_ram_store_u32_flow), 0x2500

#if defined(__ELF__)
    .section .note.GNU-stack,"",@progbits
#endif
