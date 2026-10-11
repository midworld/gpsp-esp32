/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "rv32_codegen.h"
#include "rv32_register_map.h"
#include "rv32_stub.h"
#include "rv32_thumb_memory.h"
#include "rv32_thumb_ram_helper.h"

EXT_RAM_BSS_ATTR uint8_t ewram[0x80000];
EXT_RAM_BSS_ATTR uint8_t iwram[0x10000];
EXT_RAM_BSS_ATTR uint8_t rom_page[0x8000];
EXT_RAM_BSS_ATTR uint8_t *memory_map_read[8 * 1024];
uint16_t io_registers[512] __attribute__((aligned(4)));
uint32_t gpsp_rv32_io_read_mask16[512];
uint32_t gpsp_rv32_io_read_mask32[256];
uint32_t gpsp_rv32_io_write_mask16[512];
uint32_t gpsp_rv32_io_write_mask32[256];
uint32_t gamepak_size;
uint8_t ws_cyc_seq[16][2];
uint8_t ws_cyc_nseq[16][2];

/* Single-access fixtures must never enter the multi-transfer helper. */
uint64_t gpsp_rv32_thumb_multi_slow(uint32_t *regs, uint32_t *cycles,
        uint32_t opcode, uint32_t unused)
{
    (void)regs;
    (void)cycles;
    (void)opcode;
    (void)unused;
    abort();
}

extern void gpsp_rv32_test_ram_u8(void);
extern void gpsp_rv32_test_ram_u16(void);
extern void gpsp_rv32_test_ram_u32(void);
extern void gpsp_rv32_test_ram_s8(void);
extern void gpsp_rv32_test_ram_s16(void);
extern void gpsp_rv32_test_ram_store_u8(void);
extern void gpsp_rv32_test_ram_store_u16(void);
extern void gpsp_rv32_test_ram_store_u32(void);

static unsigned callback_calls;
static unsigned callback_errors;
static unsigned expected_kind;
static uint32_t expected_address;
static uint32_t callback_value;
static uint32_t expected_store_value;
static int callback_exit;
static uint8_t callback_seq_update;

static unsigned fetch_wait(void)
{
    return 2;
}

static uint64_t test_callback(uint32_t *regs, uint32_t *cycles,
        uint32_t address, uint32_t continuation, unsigned kind)
{
    uintptr_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    callback_calls++;
    int store = kind >= GPSP_RV32_MEM_STORE_U8;
    if ((sp & 15) || kind != expected_kind || address != expected_address ||
        (store ? continuation != expected_store_value ||
                regs[1] != expected_store_value :
                !continuation || regs[1] != 0xdeadbeef) ||
            regs[0] != address ||
            regs[15] != 0x08000000 || regs[16] != 0xa000001f ||
            *cycles != 100)
        callback_errors++;
    *cycles -= 7;
    if (callback_seq_update)
        ws_cyc_seq[8][0] = callback_seq_update;
    if (callback_exit)
    {
        regs[15] += 2;
        return GPSP_RV32_HELPER_EXIT | callback_value;
    }
    return callback_value;
}

#define CALLBACK(name, kind)                                                 \
static uint64_t name(uint32_t *regs, uint32_t *cycles,                       \
        uint32_t address, uint32_t continuation)                             \
{                                                                            \
    return test_callback(regs, cycles, address, continuation, kind);         \
}

CALLBACK(test_u8, GPSP_RV32_MEM_LOAD_U8)
CALLBACK(test_u16, GPSP_RV32_MEM_LOAD_U16)
CALLBACK(test_u32, GPSP_RV32_MEM_LOAD_U32)
CALLBACK(test_s8, GPSP_RV32_MEM_LOAD_S8)
CALLBACK(test_s16, GPSP_RV32_MEM_LOAD_S16)
CALLBACK(test_store_u8, GPSP_RV32_MEM_STORE_U8)
CALLBACK(test_store_u16, GPSP_RV32_MEM_STORE_U16)
CALLBACK(test_store_u32, GPSP_RV32_MEM_STORE_U32)

const gpsp_rv32_helper_fn
        gpsp_rv32_thumb_memory_helpers[GPSP_RV32_MEM_KIND_COUNT] = {
    test_u8, test_u16, test_u32, test_s8, test_s16,
    test_store_u8, test_store_u16, test_store_u32,
};

static int test_load(void (*block)(void), uint32_t address,
        uint32_t value, uint32_t access_cycles, int expect_callback)
{
    uint32_t regs[64] = {0};
    regs[0] = address;
    regs[1] = 0xdeadbeef;
    regs[15] = 0x08000000;
    regs[16] = 0xa000001f;
    regs[17] = 0x1f;
    unsigned before = callback_calls;
    uint32_t remaining = gpsp_rv32_run_block(regs, 100, block);
    return remaining == 100 - access_cycles -
            (callback_exit ? 0 : fetch_wait()) && regs[0] == address &&
            regs[1] == value && regs[15] == 0x08000002 &&
            regs[16] == 0xa000001f &&
            callback_calls - before == (unsigned)expect_callback &&
            !callback_errors;
}

static int test_store(void (*block)(void), uint32_t address,
        uint32_t value, uint32_t access_cycles, int expect_callback)
{
    uint32_t regs[64] = {0};
    regs[0] = address;
    regs[1] = value;
    regs[15] = 0x08000000;
    regs[16] = 0xa000001f;
    regs[17] = 0x1f;
    unsigned before = callback_calls;
    uint32_t remaining = gpsp_rv32_run_block(regs, 100, block);
    return remaining == 100 - access_cycles -
            (callback_exit ? 0 : fetch_wait()) && regs[0] == address &&
            regs[1] == value && regs[15] == 0x08000002 &&
            regs[16] == 0xa000001f &&
            callback_calls - before == (unsigned)expect_callback &&
            !callback_errors;
}

static int test_store_fallback(void (*block)(void), unsigned kind,
        uint32_t address, uint32_t value, int exit_block)
{
    expected_kind = kind;
    expected_address = address;
    expected_store_value = value;
    callback_value = 0;
    callback_exit = exit_block;
    return test_store(block, address, value, 7, 1);
}

static int test_guard(void (*block)(void), uint32_t address,
        uint32_t value, uint32_t cycles)
{
    uint32_t regs[64] = {0};
    regs[0] = address;
    regs[1] = value;
    regs[15] = 0x08000000;
    regs[16] = 0xa000001f;
    regs[17] = 0x1f;
    uint8_t old_ram = ewram[0];
    unsigned old_calls = callback_calls;
    uint32_t remaining = gpsp_rv32_run_block(regs, cycles, block);
    return remaining == cycles && regs[0] == address &&
            regs[1] == value && regs[15] == 0x08000000 &&
            regs[16] == 0xa000001f && regs[17] == 0x1f &&
            ewram[0] == old_ram && callback_calls == old_calls &&
            !callback_errors;
}

static int test_exec_site(void)
{
#ifdef MALLOC_CAP_EXEC
    uint8_t *memory = heap_caps_aligned_alloc(64, 64,
            MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    if (!memory)
        return 0;

    uintptr_t helper = (uintptr_t)gpsp_rv32_ram_load_u8;
    uint32_t upper = ((uint32_t)helper + 0x800u) >> 12;
    int low = (int)(helper & 0xfffu);
    if (low >= 0x800)
        low -= 0x1000;

    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, memory, 64);
    int pass = gpsp_rv32_addi(&code, A1, guest_reg_map[0], 0) &&
            gpsp_rv32_lui(&code, RV_TMP1, upper) &&
            gpsp_rv32_addi(&code, RV_TMP1, RV_TMP1, low) &&
            gpsp_rv32_jalr(&code, RV_TMP0, RV_TMP1, 0);
    if (pass)
    {
        pass = gpsp_rv32_addi(&code, RV_BUDGET, RV_BUDGET, -2);
        pass = pass && gpsp_rv32_addi(&code, guest_reg_map[1], A0, 0) &&
                gpsp_rv32_jalr(&code, X0, RA, 0);
    }
    if (pass)
    {
        esp_err_t err = esp_cache_msync(memory, 64,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        pass = err == ESP_OK || err == ESP_ERR_NOT_SUPPORTED;
    }
    if (pass)
    {
        esp_err_t err = esp_cache_msync(memory, 64,
                ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
        pass = err == ESP_OK || err == ESP_ERR_NOT_SUPPORTED;
    }
    if (pass)
    {
        __asm__ volatile("fence.i" ::: "memory");
        pass = test_load((void (*)(void))memory,
                0x02040000, 0x81, 3, 0);
    }
    heap_caps_free(memory);
    return pass;
#else
    return 0;
#endif
}

void app_main(void)
{
    int pass = 1;
    memset(ewram, 0, sizeof(ewram));
    memset(iwram, 0, sizeof(iwram));
    ws_cyc_seq[8][0] = 2;
    ewram[0] = 0x81;
    ewram[0x3ffff] = 0x91;
    ewram[0x3fffc] = 0x78;
    ewram[0x3fffd] = 0x56;
    ewram[0x3fffe] = 0x34;
    ewram[0x3ffff] = 0x12;
    iwram[0x8000] = 0x34;
    iwram[0x8001] = 0x80;
    iwram[0xffff] = 0x91;

    pass &= test_load(gpsp_rv32_test_ram_u8,
            0x02040000, 0x81, 3, 0);
    pass &= test_guard(gpsp_rv32_test_ram_u8,
            0x02040000, 0xdeadbeef, 0);
    pass &= test_guard(gpsp_rv32_test_ram_u8,
            0x04000010, 0xdeadbeef, UINT32_MAX);
    pass &= test_exec_site();
    pass &= test_load(gpsp_rv32_test_ram_s8,
            0x03ffffff, 0xffffff91, 1, 0);
    pass &= test_load(gpsp_rv32_test_ram_u16,
            0x03008000, 0x8034, 1, 0);
    pass &= test_load(gpsp_rv32_test_ram_s16,
            0x03008000, 0xffff8034, 1, 0);
    pass &= test_load(gpsp_rv32_test_ram_u32,
            0x0207fffc, 0x12345678, 6, 0);
    pass &= test_load(gpsp_rv32_test_ram_u32,
            0x03008000, 0x8034, 1, 0);

    expected_kind = GPSP_RV32_MEM_LOAD_U32;
    expected_address = 0x08000004;
    callback_value = 0x12345678;
    pass &= test_load(gpsp_rv32_test_ram_u32,
            expected_address, callback_value, 7, 1);
    callback_seq_update = 5;
    pass &= test_load(gpsp_rv32_test_ram_u32,
            expected_address, callback_value, 7, 1);
    callback_seq_update = 0;
    ws_cyc_seq[8][0] = 2;
    rom_page[0x100] = 0x78;
    rom_page[0x101] = 0x56;
    rom_page[0x102] = 0x34;
    rom_page[0x103] = 0x12;
    gamepak_size = 0x8000;
    memory_map_read[0x08000000u >> 15] = rom_page;
    ws_cyc_nseq[8][1] = 9;
    pass &= test_load(gpsp_rv32_test_ram_u32,
            0x08000100, 0x12345678, 9, 0);
    expected_address = 0x0a000100;
    callback_value = 0xaabbccdd;
    pass &= test_load(gpsp_rv32_test_ram_u32,
            expected_address, callback_value, 7, 1);
    expected_address = 0x08000101;
    pass &= test_load(gpsp_rv32_test_ram_u32,
            expected_address, callback_value, 7, 1);
    expected_address = 0x02000001;
    callback_value = 0x78563412;
    pass &= test_load(gpsp_rv32_test_ram_u32,
            expected_address, callback_value, 7, 1);
    expected_kind = GPSP_RV32_MEM_LOAD_S16;
    expected_address = 0x03000001;
    callback_value = 0xffffff80;
    pass &= test_load(gpsp_rv32_test_ram_s16,
            expected_address, callback_value, 7, 1);

    /* Masked I/O loads skip the helper; a zero mask keeps it. */
    io_registers[3] = 0xa5f0;
    io_registers[4] = 0x8123;
    gpsp_rv32_io_read_mask16[3] = 0x7fff;
    gpsp_rv32_io_read_mask16[4] = 0xffff;
    gpsp_rv32_io_read_mask32[1] = 0x7fff7fff;
    pass &= test_load(gpsp_rv32_test_ram_u16,
            0x04000006, 0x25f0, 1, 0);
    pass &= test_load(gpsp_rv32_test_ram_s16,
            0x04000008, 0xffff8123, 1, 0);
    pass &= test_load(gpsp_rv32_test_ram_u8,
            0x04000007, 0x25, 1, 0);
    pass &= test_load(gpsp_rv32_test_ram_s8,
            0x04000009, 0xffffff81, 1, 0);
    pass &= test_load(gpsp_rv32_test_ram_u32,
            0x04000004, 0x25f00000, 1, 0);

    expected_kind = GPSP_RV32_MEM_LOAD_U8;
    expected_address = 0x04000010;
    callback_value = 0x42;
    callback_exit = 1;
    pass &= test_load(gpsp_rv32_test_ram_u8,
            expected_address, 0xdeadbeef, 7, 1);

    callback_exit = 0;
    pass &= test_store(gpsp_rv32_test_ram_store_u8,
            0x02040000, 0x5a, 3, 0);
    pass &= ewram[0] == 0x5a;
    pass &= test_store(gpsp_rv32_test_ram_store_u8,
            0x02000101, 0x6b, 3, 0);
    pass &= ewram[0x100] == 0 && ewram[0x101] == 0x6b;
    pass &= test_guard(gpsp_rv32_test_ram_store_u8,
            0x02040000, 0x43, 0);
    pass &= test_guard(gpsp_rv32_test_ram_store_u8,
            0x04000010, 0x43, UINT32_MAX);
    pass &= test_store(gpsp_rv32_test_ram_store_u8,
            0x03ffffff, 0x96, 1, 0);
    pass &= iwram[0xffff] == 0x96;
    pass &= test_store(gpsp_rv32_test_ram_store_u16,
            0x02040003, 0x1234, 3, 0);
    pass &= ewram[2] == 0x34 && ewram[3] == 0x12;
    pass &= test_store(gpsp_rv32_test_ram_store_u16,
            0x03018003, 0xabcd, 1, 0);
    pass &= iwram[0x8002] == 0xcd && iwram[0x8003] == 0xab;
    pass &= test_store(gpsp_rv32_test_ram_store_u32,
            0x0207ffff, 0x12345678, 6, 0);
    pass &= ewram[0x3fffc] == 0x78 && ewram[0x3fffd] == 0x56 &&
            ewram[0x3fffe] == 0x34 && ewram[0x3ffff] == 0x12;
    pass &= test_store(gpsp_rv32_test_ram_store_u32,
            0x0301ffff, 0x44332211, 1, 0);
    pass &= iwram[0xfffc] == 0x11 && iwram[0xfffd] == 0x22 &&
            iwram[0xfffe] == 0x33 && iwram[0xffff] == 0x44;

    ewram[0x40000 + 0x102] = 1;
    pass &= test_store_fallback(gpsp_rv32_test_ram_store_u32,
            GPSP_RV32_MEM_STORE_U32, 0x02000100, 0x12345678, 0);
    ewram[0x40000 + 0x102] = 0;
    ewram[0x40000 + 0x100] = 1;
    pass &= test_store_fallback(gpsp_rv32_test_ram_store_u32,
            GPSP_RV32_MEM_STORE_U32, 0x02000100, 0x12345678, 0);
    ewram[0x40000 + 0x100] = 0;
    iwram[0x102] = 1;
    pass &= test_store_fallback(gpsp_rv32_test_ram_store_u32,
            GPSP_RV32_MEM_STORE_U32, 0x03000100, 0x12345678, 0);
    iwram[0x102] = 0;
    iwram[0x100] = 1;
    pass &= test_store_fallback(gpsp_rv32_test_ram_store_u8,
            GPSP_RV32_MEM_STORE_U8, 0x03000101, 0x5a, 0);
    iwram[0x100] = 0;

    /* Plain I/O stores apply their mask without the helper. */
    gpsp_rv32_io_write_mask16[8] = 0x01ff;
    gpsp_rv32_io_write_mask32[8] = 0xffff3fff;
    pass &= test_store(gpsp_rv32_test_ram_store_u16,
            0x04000011, 0xabcd, 1, 0);
    pass &= io_registers[8] == 0x01cd;
    pass &= test_store(gpsp_rv32_test_ram_store_u32,
            0x04000022, 0x87654321, 1, 0);
    pass &= io_registers[16] == 0x0321 && io_registers[17] == 0x8765;

    /* IF acknowledges without the helper. IE and IME go to it only when the
       write makes an IRQ deliverable (CPSR I is clear here). */
    io_registers[0x101] = 0x000f;
    pass &= test_store(gpsp_rv32_test_ram_store_u16,
            0x04000202, 0x0005, 1, 0);
    pass &= io_registers[0x101] == 0x000a;
    io_registers[0x104] = 0;
    pass &= test_store(gpsp_rv32_test_ram_store_u16,
            0x04000200, 0x0002, 1, 0);
    pass &= io_registers[0x100] == 0x0002;
    io_registers[0x104] = 1;
    pass &= test_store_fallback(gpsp_rv32_test_ram_store_u16,
            GPSP_RV32_MEM_STORE_U16, 0x04000200, 0x0002, 0);
    io_registers[0x100] = 0x0002;
    pass &= test_store_fallback(gpsp_rv32_test_ram_store_u16,
            GPSP_RV32_MEM_STORE_U16, 0x04000208, 0x0001, 0);
    io_registers[0x100] = 0;
    io_registers[0x101] = 0;
    io_registers[0x104] = 0;
    pass &= test_store_fallback(gpsp_rv32_test_ram_store_u16,
            GPSP_RV32_MEM_STORE_U16, 0x04000012, 0x1357, 0);
    pass &= test_store_fallback(gpsp_rv32_test_ram_store_u8,
            GPSP_RV32_MEM_STORE_U8, 0x04000010, 0x62, 0);
    pass &= test_store_fallback(gpsp_rv32_test_ram_store_u16,
            GPSP_RV32_MEM_STORE_U16, 0x05000001, 0x2468, 0);
    pass &= test_store_fallback(gpsp_rv32_test_ram_store_u32,
            GPSP_RV32_MEM_STORE_U32, 0x06000003, 0x12345678, 0);
    pass &= test_store_fallback(gpsp_rv32_test_ram_store_u16,
            GPSP_RV32_MEM_STORE_U16, 0x07000000, 0x9753, 0);
    pass &= test_store_fallback(gpsp_rv32_test_ram_store_u8,
            GPSP_RV32_MEM_STORE_U8, 0x08000000, 0x45, 0);
    pass &= test_store_fallback(gpsp_rv32_test_ram_store_u16,
            GPSP_RV32_MEM_STORE_U16, 0x0d000000, 0x1, 0);
    pass &= test_store_fallback(gpsp_rv32_test_ram_store_u32,
            GPSP_RV32_MEM_STORE_U32, 0x0e000000, 0x87654321, 1);

    printf("RV32 THUMB RAM HELPER RESULT: %s (calls=%u errors=%u)\n",
            pass ? "PASS" : "FAIL", callback_calls, callback_errors);
    printf("RV32 THUMB RAM HELPER COMPLETE\n");
}
