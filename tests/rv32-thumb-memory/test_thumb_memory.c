/* Thumb memory bridge timing and state tests.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "common.h"
#include "rv32_thumb_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

u32 reg[64];
u8 ewram[0x80000];
u8 iwram[0x10000];
u8 ws_cyc_seq[16][2];
u8 ws_cyc_nseq[16][2];
u32 idle_loop_target_pc = 0xffffffff;

static u32 seen_address;
static u32 seen_pc;
static u32 seen_value;
static u32 seen_kind;
static cpu_alert_type write_alert;
static bool update_waitcnt;
static bool absorb_writes;
static u32 absorb_args[4];

/* Dynamic immediates live in the dynarec; record the query instead. */
bool gpsp_rv32_code_write_absorbed(u32 address, unsigned bytes, u32 before,
        u32 after)
{
    absorb_args[0] = address;
    absorb_args[1] = bytes;
    absorb_args[2] = before;
    absorb_args[3] = after;
    return absorb_writes;
}

static void seen(u32 kind, u32 address, u32 value)
{
    seen_kind = kind;
    seen_address = address;
    seen_pc = reg[REG_PC];
    seen_value = value;
}

u32 read_memory8(u32 address)
{
    seen(GPSP_RV32_MEM_LOAD_U8, address, 0);
    return 0x80;
}

u32 read_memory8s(u32 address)
{
    seen(GPSP_RV32_MEM_LOAD_S8, address, 0);
    return 0xffffff80;
}

u32 read_memory16(u32 address)
{
    seen(GPSP_RV32_MEM_LOAD_U16, address, 0);
    if ((address >> 24) == 2)
    {
        unsigned offset = (address & 0x3ffffu) & ~1u;
        u32 value = ewram[offset] | ((u32)ewram[offset + 1] << 8);
        return (address & 1) ? (value >> 8) | (value << 24) : value;
    }
    return 0xabcd;
}

u32 read_memory16s(u32 address)
{
    seen(GPSP_RV32_MEM_LOAD_S16, address, 0);
    return 0xffffff80;
}

u32 read_memory32(u32 address)
{
    seen(GPSP_RV32_MEM_LOAD_U32, address, 0);
    return 0x89abcdef;
}

cpu_alert_type write_memory8(u32 address, u8 value)
{
    seen(GPSP_RV32_MEM_STORE_U8, address, value);
    return write_alert;
}

cpu_alert_type write_memory16(u32 address, u16 value)
{
    seen(GPSP_RV32_MEM_STORE_U16, address, value);
    return write_alert;
}

cpu_alert_type write_memory32(u32 address, u32 value)
{
    seen(GPSP_RV32_MEM_STORE_U32, address, value);
    if (update_waitcnt)
        ws_cyc_seq[8][0] = 5;
    return write_alert;
}

static uint64_t memory_access(gpsp_rv32_memory_kind kind, u32 *cycles,
        u32 address, u32 value)
{
    return gpsp_rv32_thumb_memory_helpers[kind](reg, cycles,
            address, value);
}

static void reset_state(void)
{
    memset(reg, 0, sizeof(reg));
    memset(ewram + 0x40000, 0, 0x40000);
    memset(iwram, 0, 0x8000);
    memset(ws_cyc_seq, 0, sizeof(ws_cyc_seq));
    memset(ws_cyc_nseq, 0, sizeof(ws_cyc_nseq));
    seen_kind = GPSP_RV32_MEM_KIND_COUNT;
    write_alert = CPU_ALERT_NONE;
    update_waitcnt = false;
    absorb_writes = false;
    memset(absorb_args, 0, sizeof(absorb_args));
    idle_loop_target_pc = 0xffffffff;
    CHECK(gpsp_rv32_take_memory_alert() == 0);
}

static void test_rom_load_and_io_pc(void)
{
    reset_state();
    reg[REG_PC] = 0x08000100;
    ws_cyc_nseq[8][1] = 7;
    u32 cycles = 50;
    uint64_t result = memory_access(GPSP_RV32_MEM_LOAD_U32, &cycles,
            0x08000003, 0);
    CHECK(result == 0x89abcdef);
    CHECK(cycles == 43);
    CHECK(seen_kind == GPSP_RV32_MEM_LOAD_U32);
    CHECK(seen_address == 0x08000003);
    CHECK(seen_pc == 0x08000102);
    CHECK(reg[REG_PC] == 0x08000100);

    ws_cyc_nseq[4][0] = 2;
    result = memory_access(GPSP_RV32_MEM_LOAD_U16, &cycles,
            0x04000010, 0);
    CHECK(result == 0xabcd);
    CHECK(cycles == 41);
    CHECK(seen_pc == 0x08000100);
    CHECK(reg[REG_PC] == 0x08000100);
}

static void test_signed_load_and_store_alignment(void)
{
    reset_state();
    reg[REG_PC] = 0x08000100;
    u32 cycles = 30;
    CHECK(memory_access(GPSP_RV32_MEM_LOAD_S16, &cycles,
            0x02000001, 0) == 0xffffff80);
    CHECK(seen_kind == GPSP_RV32_MEM_LOAD_S16);
    CHECK(seen_address == 0x02000001);
    CHECK(memory_access(GPSP_RV32_MEM_STORE_U32, &cycles,
            0x03000003, 0x12345678) == 0);
    CHECK(seen_address == 0x03000000);
    CHECK(seen_value == 0x12345678);
    CHECK(reg[REG_PC] == 0x08000100);
}

static void test_odd_ewram_halfword_load(void)
{
    reset_state();
    reg[REG_PC] = 0x08000100;
    ewram[0] = 0x11;
    ewram[1] = 0x22;
    u32 cycles = 30;
    CHECK(memory_access(GPSP_RV32_MEM_LOAD_U16, &cycles,
            0x02000001, 0) == 0x22);
    CHECK(seen_address == 0x02000001);
    CHECK(reg[REG_PC] == 0x08000100);
}

static void test_waitcnt_and_alert(void)
{
    reset_state();
    reg[REG_PC] = 0x08000100;
    ws_cyc_nseq[4][1] = 4;
    ws_cyc_seq[8][0] = 3;
    update_waitcnt = true;
    u32 cycles = 40;
    CHECK(memory_access(GPSP_RV32_MEM_STORE_U32, &cycles,
            0x04000204, 0x321) == 0);
    CHECK(cycles == 36);           /* Fetch cost is emitted after the call. */
    CHECK(ws_cyc_seq[8][0] == 5);
    CHECK(reg[REG_PC] == 0x08000100);

    write_alert = CPU_ALERT_IRQ | CPU_ALERT_SMC;
    uint64_t result = memory_access(GPSP_RV32_MEM_STORE_U32, &cycles,
            0x04000204, 0x321);
    CHECK(result & GPSP_RV32_HELPER_EXIT);
    CHECK(cycles == 27);           /* Old 36 - access 4 - new fetch 5. */
    CHECK(reg[REG_PC] == 0x08000102);
    CHECK(gpsp_rv32_take_memory_alert() == (CPU_ALERT_IRQ | CPU_ALERT_SMC));
    CHECK(gpsp_rv32_take_memory_alert() == 0);

    idle_loop_target_pc = 0x08000104;
    result = memory_access(GPSP_RV32_MEM_STORE_U32, &cycles,
            0x04000204, 0x321);
    CHECK(result & GPSP_RV32_HELPER_EXIT);
    CHECK(cycles == 0);
    CHECK(reg[REG_PC] == idle_loop_target_pc);
    CHECK(gpsp_rv32_take_memory_alert() == (CPU_ALERT_IRQ | CPU_ALERT_SMC));
}

static void test_ram_code_write_alert(void)
{
    reset_state();
    reg[REG_PC] = 0x08000100;
    ws_cyc_seq[8][0] = 1;
    u32 cycles = 30;
    iwram[0x100 + 2] = 1;
    CHECK(gpsp_rv32_store_hits_code(0x03000100, 4));
    CHECK(!gpsp_rv32_store_hits_code(0x03000104, 4));
    CHECK(memory_access(GPSP_RV32_MEM_STORE_U32, &cycles,
            0x03000100, 0x12345678) & GPSP_RV32_HELPER_EXIT);
    CHECK(gpsp_rv32_take_memory_alert() == CPU_ALERT_SMC);
    CHECK(reg[REG_PC] == 0x08000102);

    reset_state();
    reg[REG_PC] = 0x08000100;
    ewram[0x40000 + 0x100] = 1;
    CHECK(gpsp_rv32_store_hits_code(0x02040101, 1));
    CHECK(memory_access(GPSP_RV32_MEM_STORE_U8, &cycles,
            0x02040101, 0x12) & GPSP_RV32_HELPER_EXIT);
    CHECK(gpsp_rv32_take_memory_alert() == CPU_ALERT_SMC);

    /* Rewriting the bytes already there keeps the code valid. */
    reset_state();
    reg[REG_PC] = 0x08000100;
    iwram[0x100 + 2] = 1;
    memcpy(iwram + 0x8000 + 0x100, "\x78\x56\x34\x12", 4);
    CHECK(!gpsp_rv32_store_changes_code(0x03000100, 4, 0x12345678));
    CHECK(gpsp_rv32_store_changes_code(0x03000100, 4, 0x12345679));
    CHECK(!(memory_access(GPSP_RV32_MEM_STORE_U32, &cycles,
            0x03000100, 0x12345678) & GPSP_RV32_HELPER_EXIT));
    CHECK(gpsp_rv32_take_memory_alert() == CPU_ALERT_NONE);
    CHECK(!gpsp_rv32_store_changes_code(0x03000101, 1, 0xab56));

    /* A change that dynamic code absorbs needs no exit. */
    CHECK(gpsp_rv32_store_changes_code(0x03000103, 1, 0x13));
    CHECK(absorb_args[0] == 0x03000103 && absorb_args[1] == 1 &&
            absorb_args[2] == 0x12 && absorb_args[3] == 0x13);
    absorb_writes = true;
    CHECK(!gpsp_rv32_store_changes_code(0x03000103, 1, 0x13));
    CHECK(!(memory_access(GPSP_RV32_MEM_STORE_U32, &cycles,
            0x03000100, 0x12345679) & GPSP_RV32_HELPER_EXIT));
    CHECK(gpsp_rv32_take_memory_alert() == CPU_ALERT_NONE);
    CHECK(absorb_args[0] == 0x03000100 && absorb_args[1] == 4 &&
            absorb_args[2] == 0x12345678 && absorb_args[3] == 0x12345679);
}

static void test_multi_slow_push_and_pop(void)
{
    reset_state();
    reg[REG_PC] = 0x08000100;
    reg[REG_SP] = 0x04000010;
    reg[0] = 0x11223344;
    reg[1] = 0x55667788;
    ws_cyc_seq[4][1] = 3;
    ws_cyc_seq[8][0] = 2;
    write_alert = CPU_ALERT_IRQ;
    u32 cycles = 50;
    CHECK(gpsp_rv32_thumb_multi_slow(reg, &cycles, 0xb403, 0) &
            GPSP_RV32_HELPER_EXIT);
    CHECK(reg[REG_SP] == 0x04000008);
    CHECK(reg[REG_PC] == 0x08000102);
    CHECK(cycles == 42);
    CHECK(seen_address == 0x0400000c && seen_value == 0x55667788);
    CHECK(gpsp_rv32_take_memory_alert() == CPU_ALERT_IRQ);

    reset_state();
    reg[REG_PC] = 0x08000100;
    reg[REG_SP] = 0x08000000;
    ws_cyc_seq[8][1] = 4;
    ws_cyc_seq[9][0] = 5;
    cycles = 50;
    CHECK(gpsp_rv32_thumb_multi_slow(reg, &cycles, 0xbd01, 0) &
            GPSP_RV32_HELPER_EXIT);
    CHECK(reg[0] == 0x89abcdef);
    CHECK(reg[REG_SP] == 0x08000008);
    CHECK(reg[REG_PC] == 0x89abcdee);
    CHECK(cycles == 37);
    CHECK(seen_address == 0x08000004);
}

int main(void)
{
    test_rom_load_and_io_pc();
    test_signed_load_and_store_alignment();
    test_odd_ewram_halfword_load();
    test_waitcnt_and_alert();
    test_ram_code_write_alert();
    test_multi_slow_push_and_pop();
    puts("rv32-thumb-memory: ok");
    return 0;
}
