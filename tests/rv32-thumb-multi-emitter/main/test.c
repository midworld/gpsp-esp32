/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "rv32_block_emit.h"
#include "rv32_block_link.h"
#include "rv32_arm_memory.h"
#include "rv32_arm_system.h"
#include "rv32_stub.h"
#include "rv32_thumb_memory.h"
#include "../../rv32-golden.h"

#define CODE_SIZE 4096
#define PC 0x08000000u

EXT_RAM_BSS_ATTR uint8_t ewram[0x80000];
EXT_RAM_BSS_ATTR uint8_t iwram[0x10000];
EXT_RAM_BSS_ATTR uint8_t *memory_map_read[8 * 1024];
EXT_RAM_BSS_ATTR uint32_t rom_page[0x8000 / 4];
uint32_t gamepak_size;
uint32_t gpsp_rv32_branch_cache[1024 * 2];
uint8_t ws_cyc_seq[16][2];
uint8_t ws_cyc_nseq[16][2];
uint16_t io_registers[512];
uint32_t gpsp_rv32_io_read_mask16[512];
uint32_t gpsp_rv32_io_read_mask32[256];
uint32_t gpsp_rv32_io_write_mask16[512];
uint32_t gpsp_rv32_io_write_mask32[256];
uint32_t idle_loop_target_pc = UINT32_MAX;
uint8_t *rom_translation_cache;
uint8_t *rom_translation_ptr;
uint8_t *last_rom_translation_ptr;
uint32_t rom_cache_watermark;

gpsp_rv32_arm_memory_result gpsp_rv32_emit_arm_memory(
        gpsp_rv32_code *code, uint32_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_MEMORY_UNSUPPORTED;
}

gpsp_rv32_arm_system_result gpsp_rv32_emit_arm_system(
        gpsp_rv32_code *code, uint32_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_SYSTEM_UNSUPPORTED;
}

gpsp_rv32_arm_system_result gpsp_rv32_emit_thumb_swi(
        gpsp_rv32_code *code, uint16_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_SYSTEM_UNSUPPORTED;
}

static unsigned fallback_calls;
static unsigned fallback_errors;

/* No case should reach the C memory path; record it as an error. */
static uint64_t unexpected_memory(uint32_t *regs, uint32_t *cycles,
        uint32_t address, uint32_t value)
{
    (void)cycles;
    (void)address;
    (void)value;
    fallback_errors++;
    regs[15] += 2;
    return GPSP_RV32_HELPER_EXIT;
}

const gpsp_rv32_helper_fn
        gpsp_rv32_thumb_memory_helpers[GPSP_RV32_MEM_KIND_COUNT] = {
    unexpected_memory, unexpected_memory, unexpected_memory,
    unexpected_memory, unexpected_memory, unexpected_memory,
    unexpected_memory, unexpected_memory,
};

uint64_t gpsp_rv32_interpret_one_helper(uint32_t *regs, uint32_t *cycles,
        uint32_t arg0, uint32_t arg1)
{
    uintptr_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    if ((sp & 15) || arg0 || arg1 || regs[15] != PC ||
        !(regs[16] & 0x20) || *cycles != 100)
        fallback_errors++;
    fallback_calls++;
    regs[15] += 2;
    *cycles -= 7;
    return GPSP_RV32_HELPER_EXIT;
}

uint64_t gpsp_rv32_thumb_multi_slow(uint32_t *regs, uint32_t *cycles,
        uint32_t opcode, uint32_t unused)
{
    uintptr_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    if ((sp & 15) || !opcode || unused || regs[15] != PC ||
        !(regs[16] & 0x20) || *cycles != 100)
        fallback_errors++;
    fallback_calls++;
    regs[15] += 2;
    *cycles -= 7;
    return GPSP_RV32_HELPER_EXIT;
}

static uint32_t *ew_word(unsigned offset)
{
    return (uint32_t *)(void *)(ewram + offset);
}

static uint32_t *iw_word(unsigned offset)
{
    return (uint32_t *)(void *)(iwram + 0x8000 + offset);
}

static int sync_code(void *memory, size_t size)
{
    esp_err_t err = esp_cache_msync(memory, size,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        return 0;
    err = esp_cache_msync(memory, size,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
        return 0;
    __asm__ volatile("fence.i" ::: "memory");
    return 1;
}

static int run_at_flags(void *memory, uint32_t pc, uint16_t opcode,
        uint8_t flags, uint32_t *regs, uint32_t cycles, int want_exit,
        uint32_t *remaining, unsigned *words)
{
    uint8_t *ptr = memory;
    bool ends = false;
    gpsp_rv32_block_link link;
    if (!gpsp_rv32_emit_block_instruction(&ptr,
            (uint8_t *)memory + CODE_SIZE, pc, opcode, true, true, flags,
            false, &ends, &link) || ends != !!want_exit || link.site)
    {
        printf("emit failed: %04x end=%d want=%d\n", opcode, ends, want_exit);
        return 0;
    }
    if (words)
        *words = (unsigned)(ptr - (uint8_t *)memory) / 4;
    if (!ends && !gpsp_rv32_emit_block_return(&ptr,
            (uint8_t *)memory + CODE_SIZE))
        return 0;
    rv32_golden_dump("thumb", memory, CODE_SIZE, memory, ptr);
    if (!sync_code(memory, CODE_SIZE))
        return 0;
    *remaining = gpsp_rv32_run_block(regs, cycles, memory);
    return 1;
}

static int run_at(void *memory, uint32_t pc, uint16_t opcode,
        uint32_t *regs, uint32_t cycles, int want_exit, uint32_t *remaining,
        unsigned *words)
{
    return run_at_flags(memory, pc, opcode, 15, regs, cycles, want_exit,
            remaining, words);
}

static int run(void *memory, uint16_t opcode, uint32_t *regs,
        uint32_t cycles, int want_exit, uint32_t *remaining, unsigned *words)
{
    return run_at(memory, PC, opcode, regs, cycles, want_exit, remaining,
            words);
}

static void init_regs(uint32_t *regs)
{
    memset(regs, 0, 64 * sizeof(*regs));
    regs[15] = PC;
    regs[16] = 0xa000003fu;
    regs[17] = 0x1f;
}

static int test_alu_flags(void *memory)
{
    /* Mask bits: N=8, Z=4, C=2, V=1. */
    static const struct
    {
        uint16_t opcode;
        uint8_t flags;
        uint32_t left, right, cpsr, result, expected_cpsr;
    } cases[] = {
        /* BIC r0,r1 and BIC r0,r0. */
        {0x4388, 15, 0xf0f0ff00, 0x0f0f0f0f, 0x3000003f,
                0xf0f0f000, 0xb000003f},
        {0x4380, 15, 0xf0f0ff00, 0x0f0f0f0f, 0x3000003f,
                0, 0x7000003f},
        /* SUBS r0,#1: V is live; N, Z and C are preserved. */
        {0x3801, 1, 0x80000000, 0, 0xe000003f, 0x7fffffff, 0xf000003f},
        {0x3801, 1, 0, 0, 0xf000003f, 0xffffffff, 0xe000003f},
        {0x3801, 1, 0x7fffffff, 0, 0xf000003f, 0x7ffffffe, 0xe000003f},
        /* ROR r0,r1: C live, then C dead. Count uses the low eight bits. */
        {0x41c8, 15, 0x80000001, 0, 0x5000003f, 0x80000001, 0x9000003f},
        {0x41c8, 15, 0x80000001, 1, 0x5000003f, 0xc0000000, 0xb000003f},
        {0x41c8, 15, 0x80000001, 31, 0x5000003f, 3, 0x1000003f},
        {0x41c8, 15, 0x80000001, 32, 0x5000003f, 0x80000001, 0xb000003f},
        {0x41c8, 15, 0x80000001, 33, 0x5000003f, 0xc0000000, 0xb000003f},
        {0x41c8, 15, 0x80000001, 255, 0x5000003f, 3, 0x1000003f},
        {0x41c8, 15, 0x80000001, 256, 0x5000003f, 0x80000001, 0x9000003f},
        {0x41c8, 13, 0x80000001, 0, 0x5000003f, 0x80000001, 0x9000003f},
        {0x41c8, 13, 0x80000001, 1, 0x5000003f, 0xc0000000, 0x9000003f},
        {0x41c8, 13, 0x80000001, 31, 0x5000003f, 3, 0x1000003f},
        {0x41c8, 13, 0x80000001, 32, 0x5000003f, 0x80000001, 0x9000003f},
        {0x41c8, 13, 0x80000001, 33, 0x5000003f, 0xc0000000, 0x9000003f},
        {0x41c8, 13, 0x80000001, 255, 0x5000003f, 3, 0x1000003f},
        {0x41c8, 13, 0x80000001, 256, 0x5000003f, 0x80000001, 0x9000003f},
        /* Register shifts check C updates. */
        {0x4088, 15, 0x80000001, 1, 0x1000003f, 2, 0x3000003f},
        {0x4088, 15, 0x00001001, 19, 0x3000003f, 0x80080000, 0x9000003f},
        {0x40c8, 15, 0x80000001, 1, 0x1000003f, 0x40000000, 0x3000003f},
        {0x4108, 15, 0x80000001, 4, 0x3000003f, 0xf8000000, 0x9000003f},
        /* LSLS r0,r1,#2, LSRS r0,r1,#1 and ASRS r0,r1,#4. */
        {0x0088, 15, 0, 0x40000001, 0x1000003f, 4, 0x3000003f},
        {0x0848, 15, 0, 0x80000001, 0x1000003f, 0x40000000, 0x3000003f},
        {0x1108, 15, 0, 0x80000008, 0x1000003f, 0xf8000000, 0xb000003f},
    };
    unsigned calls = fallback_calls;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        uint32_t regs[64], expected[18], remaining;
        init_regs(regs);
        regs[0] = cases[i].left;
        regs[1] = cases[i].right;
        regs[16] = cases[i].cpsr;
        memcpy(expected, regs, sizeof(expected));
        expected[0] = cases[i].result;
        expected[15] = PC + 2;
        expected[16] = cases[i].expected_cpsr;
        if (!run_at_flags(memory, PC, cases[i].opcode, cases[i].flags,
                regs, 100, 0, &remaining, NULL) || remaining != 98 ||
                memcmp(regs, expected, sizeof(expected)) || fallback_calls != calls)
        {
            printf("ALU flags case %u failed: opcode=%04x mask=%u\n",
                    i, cases[i].opcode, cases[i].flags);
            return 0;
        }
    }
    return 1;
}

static int test_push(void *memory)
{
    uint32_t regs[64], remaining;
    init_regs(regs);
    regs[0] = 0x11223344;
    regs[2] = 0x55667788;
    regs[13] = 0x03007f10;
    regs[14] = 0x08001111;
    unsigned calls = fallback_calls;
    return run(memory, 0xb505, regs, 100, 0, &remaining, NULL) &&
            remaining == 95 && regs[13] == 0x03007f04 &&
            regs[15] == PC + 2 && regs[16] == 0xa000003f &&
            *iw_word(0x7f04) == 0x11223344 &&
            *iw_word(0x7f08) == 0x55667788 &&
            *iw_word(0x7f0c) == 0x08001111 &&
            fallback_calls == calls;
}

static int test_pop_pc(void *memory)
{
    uint32_t regs[64], remaining;
    init_regs(regs);
    regs[13] = 0x03007f00;
    *iw_word(0x7f00) = 0x12345678;
    *iw_word(0x7f04) = 0x87654321;
    *iw_word(0x7f08) = 0x08001235;
    unsigned calls = fallback_calls;
    return run(memory, 0xbd81, regs, 100, 1, &remaining, NULL) &&
            remaining == 95 && regs[0] == 0x12345678 &&
            regs[7] == 0x87654321 && regs[13] == 0x03007f0c &&
            regs[15] == 0x08001234 && regs[16] == 0xa000003f &&
            fallback_calls == calls;
}

static int test_stmia_base_first(void *memory)
{
    uint32_t regs[64], remaining;
    init_regs(regs);
    regs[0] = 0x02000100;
    regs[1] = 0x3456789a;
    regs[2] = 0xabcdef01;
    unsigned calls = fallback_calls;
    return run(memory, 0xc007, regs, 100, 0, &remaining, NULL) &&
            remaining == 80 && regs[0] == 0x0200010c &&
            regs[15] == PC + 2 && *ew_word(0x100) == 0x02000100 &&
            *ew_word(0x104) == 0x3456789a &&
            *ew_word(0x108) == 0xabcdef01 && fallback_calls == calls;
}

static int test_ldmia_base_in_list(void *memory)
{
    uint32_t regs[64], remaining;
    init_regs(regs);
    regs[1] = 0x03000100;
    *iw_word(0x100) = 0x10203040;
    *iw_word(0x104) = 0x50607080;
    *iw_word(0x108) = 0x90a0b0c0;
    unsigned calls = fallback_calls;
    return run(memory, 0xc90b, regs, 100, 0, &remaining, NULL) &&
            remaining == 95 && regs[0] == 0x10203040 &&
            regs[1] == 0x50607080 && regs[3] == 0x90a0b0c0 &&
            regs[15] == PC + 2 && fallback_calls == calls;
}

static int test_unaligned_base(void *memory)
{
    uint32_t regs[64], remaining;
    init_regs(regs);
    regs[0] = 0x02000101;
    regs[2] = 0x2468ace0;
    unsigned calls = fallback_calls;
    return run(memory, 0xc005, regs, 100, 0, &remaining, NULL) &&
            remaining == 86 && regs[0] == 0x02000109 &&
            *ew_word(0x100) == 0x02000101 &&
            *ew_word(0x104) == 0x2468ace0 && fallback_calls == calls;
}

static int test_fallback(void *memory, uint16_t opcode,
        unsigned base, uint32_t address, int emitted_exit)
{
    uint32_t regs[64], remaining;
    init_regs(regs);
    regs[base] = address;
    unsigned calls = fallback_calls;
    return run(memory, opcode, regs, 100, emitted_exit, &remaining, NULL) &&
            remaining == 93 && regs[base] == address &&
            regs[15] == PC + 2 && fallback_calls == calls + 1 &&
            !fallback_errors;
}

static int test_guard(void *memory, uint32_t cycles)
{
    uint32_t regs[64], remaining;
    init_regs(regs);
    regs[0] = 0x11223344;
    regs[13] = 0x03007f10;
    uint32_t old_word = *iw_word(0x7f0c);
    unsigned calls = fallback_calls;
    return run(memory, 0xb401, regs, cycles, 0, &remaining, NULL) &&
            remaining == cycles && regs[0] == 0x11223344 &&
            regs[13] == 0x03007f10 && regs[15] == PC &&
            *iw_word(0x7f0c) == old_word && fallback_calls == calls;
}

static int test_pc_targets(void *memory)
{
    uint32_t regs[64], remaining;
    unsigned calls = fallback_calls;

    init_regs(regs);
    if (!run(memory, 0xa002, regs, 100, 0, &remaining, NULL) ||
        regs[0] != PC + 12 || regs[15] != PC + 2 || remaining != 98)
        return 0;

    init_regs(regs);
    regs[0] = PC + 0x105;
    if (!run(memory, 0x4687, regs, 100, 1, &remaining, NULL) ||
        regs[15] != PC + 0x104 || remaining != 98)
        return 0; /* MOV PC,r0 */

    init_regs(regs);
    regs[0] = 0x11;
    if (!run(memory, 0x4487, regs, 100, 1, &remaining, NULL) ||
        regs[15] != PC + 0x14 || remaining != 98)
        return 0; /* ADD PC,r0 */

    init_regs(regs);
    regs[14] = PC + 0x200;
    return run(memory, 0xf801, regs, 100, 1, &remaining, NULL) &&
            regs[14] == ((PC + 2) | 1) && regs[15] == PC + 0x202 &&
            remaining == 98 && fallback_calls == calls;
}

static int test_literal_load(void *memory)
{
    uint32_t regs[64], remaining;
    unsigned calls = fallback_calls;
    memory_map_read[PC >> 15] = (uint8_t *)rom_page;
    gamepak_size = sizeof(rom_page);
    rom_page[1] = 0x12345678;
    rom_page[0x400 / 4] = 0x87654321;

    init_regs(regs);
    if (!run(memory, 0x4800, regs, 100, 0, &remaining, NULL) ||
        regs[0] != 0x12345678 || regs[15] != PC + 2 || remaining != 95)
        return 0;
    init_regs(regs);
    return run(memory, 0x48ff, regs, 100, 0, &remaining, NULL) &&
            regs[0] == 0x87654321 && regs[15] == PC + 2 &&
            remaining == 95 && fallback_calls == calls;
}

/* Code run from IWRAM reads its literals through the RAM path; the ROM
   loader would miss and reach the C path. */
static int test_ram_literal_load(void *memory)
{
    uint32_t regs[64], remaining;
    unsigned calls = fallback_calls;
    *iw_word(0x108) = 0x0badf00d;
    init_regs(regs);
    regs[15] = 0x03000100;
    return run_at(memory, 0x03000100, 0x4801, regs, 100, 0, &remaining,
            NULL) &&
            regs[0] == 0x0badf00d && regs[15] == 0x03000102 &&
            remaining < 100 && fallback_calls == calls && !fallback_errors;
}

/* Per-instruction fixed state; stack storage preserves golden addresses. */
static uint8_t *hook_fixed;
static unsigned hook_count, hook_capacity;

static void record_fixed(const gpsp_rv32_code *code)
{
    if (hook_count < hook_capacity)
        hook_fixed[hook_count] = code->fixed_depth != 0;
    hook_count++;
}

/* Count fixed runs and words when one instruction is emitted. */
static int fixed_run(void *memory, uint16_t opcode, unsigned want_runs,
        unsigned want_words)
{
    uint8_t *ptr = memory;
    bool ends = false;
    gpsp_rv32_block_link link;
    uint8_t fixed[256];
    hook_fixed = fixed;
    hook_capacity = sizeof(fixed);
    hook_count = 0;
    gpsp_rv32_test_emit_hook = record_fixed;
    bool emitted = gpsp_rv32_emit_block_instruction(&ptr,
            (uint8_t *)memory + CODE_SIZE, 0x03000100, opcode, true, true, 15,
            false, &ends, &link);
    gpsp_rv32_test_emit_hook = NULL;
    if (!emitted || hook_count > hook_capacity)
        return 0;
    unsigned runs = 0, length = 0;
    for (unsigned i = 0; i < hook_count; i++)
    {
        if (hook_fixed[i] && (!i || !hook_fixed[i - 1]))
            runs++;
        length += hook_fixed[i];
    }
    if (runs != want_runs || length != want_words)
        printf("fixed span %04x: runs=%u words=%u\n", opcode, runs, length);
    return runs == want_runs && length == want_words;
}

/* Per-instruction helpers exit on an empty budget. Their return address
   has no wait literal, so these calls need no fixed span. */
static int test_retry_spans(void *memory)
{
    return fixed_run(memory, 0x4801, 0, 0) &&  /* LDR r0, [pc, #4] */
            fixed_run(memory, 0x6001, 0, 0);       /* STR r1, [r0] */
}

static int test_static_branches(void *memory)
{
    static const struct
    {
        uint32_t opcode, target, remaining;
        int thumb, link;
    } cases[] = {
        {0xe000, PC + 4, 98, 1, 0},
        {0xe3ff, PC + 2050, 98, 1, 0},
        {0xe400, PC - 2044, 100, 1, 0},
        {0xea000000, PC + 8, 93, 0, 0},
        {0xeb000000, PC + 8, 93, 0, 1},
        {0xea000800, PC + 8200, 93, 0, 0},
        {0xea800000, PC + 8 - 0x02000000, 100, 0, 0},
    };
    unsigned calls = fallback_calls;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        uint32_t regs[64], remaining;
        init_regs(regs);
        if (!cases[i].thumb)
            regs[16] &= ~0x20u;
        uint8_t *ptr = memory;
        bool ends = false;
        gpsp_rv32_block_link link;
        if (!gpsp_rv32_emit_block_instruction(&ptr,
                (uint8_t *)memory + CODE_SIZE, PC, cases[i].opcode,
                cases[i].thumb, true, 15, false, &ends, &link) ||
                !ends || !link.site || link.target != cases[i].target ||
                link.thumb != !!cases[i].thumb ||
                (rv32_golden_dump("thumb_branch", memory, CODE_SIZE, memory, ptr),
                        !sync_code(memory, CODE_SIZE)))
            return 0;
        remaining = gpsp_rv32_run_block(regs, 100, memory);
        if (regs[15] != cases[i].target ||
            regs[14] != (cases[i].link ? PC + 4 : 0) ||
                remaining != cases[i].remaining)
            return 0;
    }
    return fallback_calls == calls;
}

static int print_sizes(void *memory)
{
    static const uint16_t opcodes[4] = {
        0xb507, 0xbd07, 0xc01e, 0xc81e,
    };
    unsigned words[4];
    /* Counts of 32-bit instructions. */
    bool compress = gpsp_rv32_test_compress;
    gpsp_rv32_test_compress = false;
    for (unsigned i = 0; i < 4; i++)
    {
        uint32_t regs[64], remaining;
        init_regs(regs);
        regs[0] = 0x02000100;
        regs[13] = 0x03007f00;
        if (!run(memory, opcodes[i], regs, 0, i == 1,
                &remaining, &words[i]))
        {
            gpsp_rv32_test_compress = compress;
            return 0;
        }
    }
    gpsp_rv32_test_compress = compress;
    printf("RV32 THUMB MULTI EMITTER WORDS: push=%u pop=%u stmia=%u ldmia=%u\n",
            words[0], words[1], words[2], words[3]);
    /* Fetch waits are captured; POP {PC} uses the shared exit. */
    return words[0] == 21 && words[1] == 21 &&
            words[2] == 21 && words[3] == 21;
}

void app_main(void)
{
    uint8_t *memory = heap_caps_aligned_alloc(64, CODE_SIZE,
            MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    if (!memory)
    {
        printf("RV32 THUMB MULTI EMITTER RESULT: FAIL (allocation)\n");
        printf("RV32 THUMB MULTI EMITTER COMPLETE\n");
        return;
    }
    memset(ewram, 0, sizeof(ewram));
    memset(iwram, 0, sizeof(iwram));
    ws_cyc_seq[8][0] = 2;
    ws_cyc_seq[8][1] = 4;
    ws_cyc_nseq[8][1] = 3;

    int pass = 1;
#define CHECK(test) do { if (!(test)) { printf("FAIL: %s\n", #test); pass = 0; } } while (0)
    CHECK(test_alu_flags(memory));
    CHECK(test_push(memory));
    CHECK(test_pop_pc(memory));
    CHECK(test_stmia_base_first(memory));
    CHECK(test_ldmia_base_in_list(memory));
    CHECK(test_unaligned_base(memory));
    CHECK(test_fallback(memory, 0xb401, 13, 0x04000010, 0));
    CHECK(test_fallback(memory, 0xc006, 0, 0x0203fffc, 0));
    ewram[0x40000 + 0x102] = 1;
    CHECK(test_fallback(memory, 0xc006, 0, 0x02000100, 0));
    ewram[0x40000 + 0x102] = 0;
    CHECK(test_fallback(memory, 0xc103, 1, 0x02000100, 1));
    CHECK(test_guard(memory, 0));
    CHECK(test_guard(memory, UINT32_MAX));
    CHECK(test_pc_targets(memory));
    CHECK(test_literal_load(memory));
    CHECK(test_ram_literal_load(memory));
    CHECK(test_retry_spans(memory));
    CHECK(test_static_branches(memory));
    CHECK(print_sizes(memory));
#undef CHECK

    printf("RV32 THUMB MULTI EMITTER RESULT: %s (slow=%u errors=%u)\n",
            pass && !fallback_errors ? "PASS" : "FAIL",
            fallback_calls, fallback_errors);
    printf("RV32 THUMB MULTI EMITTER COMPLETE\n");
    heap_caps_free(memory);
}
