/* RV32 Thumb batch timing and generated instruction count tests.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the internal emitter with low synthetic callback addresses. */
#include "rv32_block_emit.c"
#include "../rv32-rvc.h"

uint32_t idle_loop_target_pc;
uint8_t ws_cyc_seq[16][2];
uint8_t ws_cyc_nseq[16][2];

/* The block emitter links these paths, but the Bcc tests never take them. */
gpsp_rv32_arm_alu_result gpsp_rv32_emit_arm_alu_block(
        gpsp_rv32_code *code, uint32_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_ALU_UNSUPPORTED;
}

gpsp_rv32_arm_memory_result gpsp_rv32_emit_arm_memory(
        gpsp_rv32_code *code, uint32_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_MEMORY_UNSUPPORTED;
}

gpsp_rv32_arm_mul_result gpsp_rv32_emit_arm_mul(
        gpsp_rv32_code *code, uint32_t opcode, uint32_t pc)
{
    (void)code;
    (void)opcode;
    (void)pc;
    return GPSP_RV32_ARM_MUL_UNSUPPORTED;
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

const gpsp_rv32_helper_fn
        gpsp_rv32_thumb_memory_helpers[GPSP_RV32_MEM_KIND_COUNT] = {0};

#define UNUSED_RV32_ENTRY(name) void name(void) { abort(); }
UNUSED_RV32_ENTRY(gpsp_rv32_call_helper)
UNUSED_RV32_ENTRY(gpsp_rv32_interpreter_step_stub)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u8)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u16)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u32)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_s8)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_s16)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u8)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u16)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u32)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_u8)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_u16)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_u32)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_s8)
UNUSED_RV32_ENTRY(gpsp_rv32_rom_load_s16)
UNUSED_RV32_ENTRY(gpsp_rv32_thumb_multi_ram)
UNUSED_RV32_ENTRY(gpsp_rv32_thumb_multi_ram_store)
UNUSED_RV32_ENTRY(gpsp_rv32_thumb_multi_slow_bridge)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u8_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u16_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_u32_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_s8_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_load_s16_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u8_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u16_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_ram_store_u32_flow)
UNUSED_RV32_ENTRY(gpsp_rv32_thumb_pop_exit_shared)
#undef UNUSED_RV32_ENTRY

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

enum { WAIT_ADDR = 0x20001234, START_PC = 0x08000100 };

typedef enum { RAN, FALLBACK, RETURNED } run_result;
static uint32_t run_code_base;
static const uint32_t *run_stub_words;
static size_t run_stub_size;
static uint32_t run_stub_base;
static uint32_t run_last_target;

static int32_t branch_offset(uint32_t word)
{
    uint32_t imm = ((word >> 31) << 12) |
            (((word >> 7) & 1) << 11) |
            (((word >> 25) & 63) << 5) |
            (((word >> 8) & 15) << 1);
    return (int32_t)(imm << 19) >> 19;
}

/* size and fallback_at are byte offsets in words. */
static run_result run(uint32_t *words, size_t size,
        size_t fallback_at, unsigned wait, uint32_t budget,
        unsigned *executed, uint32_t *remaining, uint32_t *pc,
        const uint32_t *initial, uint32_t *final)
{
    uint32_t regs[32] = {0};
    if (initial)
        memcpy(regs, initial, sizeof(regs));
    regs[RV_BUDGET] = budget;
    regs[RV_PC] = initial && initial[RV_PC] ? initial[RV_PC] : START_PC;
    run_last_target = 0;
    size_t at = 0;
    *executed = 0;
    while (1)
    {
        size_t stub_at = run_stub_base && run_code_base ?
                run_stub_base - run_code_base : 0;
        bool in_stub = run_stub_words && at >= stub_at &&
                at - stub_at < run_stub_size;
        if (at >= size && !in_stub)
            break;
        if (at == fallback_at && !in_stub)
            goto fallback;
        uint32_t word = 0;
        unsigned length = in_stub ?
                rv32_rvc_fetch(run_stub_words, run_stub_size, at - stub_at,
                        &word) :
                rv32_rvc_fetch(words, size, at, &word);
        CHECK(length);
        if (!length)
            break;
        unsigned op = word & 0x7f;
        unsigned rd = (word >> 7) & 31;
        unsigned rs1 = (word >> 15) & 31;
        unsigned rs2 = (word >> 20) & 31;
        (*executed)++;
        if (op == 0x63)
        {
            unsigned kind = (word >> 12) & 7;
            bool take = kind == 0 ? regs[rs1] == regs[rs2] :
                    kind == 1 ? regs[rs1] != regs[rs2] :
                    kind == 4 ? (int32_t)regs[rs1] < (int32_t)regs[rs2] :
                    (int32_t)regs[rs1] >= (int32_t)regs[rs2];
            CHECK(kind == 0 || kind == 1 || kind == 4 || kind == 5);
            at = take ? at + branch_offset(word) : at + length;
            continue;
        }
        if (op == 0x67)
        {
            uint32_t target = (regs[rs1] + ((int32_t)word >> 20)) & ~1u;
            if (rd)
                regs[rd] = run_code_base + at + length;
            if (run_code_base && target >= run_code_base &&
                target - run_code_base < size)
            {
                at = target - run_code_base;
                continue;
            }
            if (run_stub_words && target >= run_stub_base &&
                target - run_stub_base < run_stub_size)
            {
                at = stub_at + (target - run_stub_base);
                continue;
            }
            run_last_target = target;
            *remaining = regs[RV_BUDGET];
            *pc = regs[RV_PC];
            if (final)
                memcpy(final, regs, sizeof(regs));
            return RETURNED;
        }
        if (op == 0x37)
            regs[rd] = word & 0xfffff000;
        else if (op == 0x17)
            regs[rd] = run_code_base + at + (word & 0xfffff000);
        else if (op == 0x13)
        {
            unsigned funct3 = (word >> 12) & 7;
            if (funct3 == 1)
                regs[rd] = regs[rs1] << ((word >> 20) & 31);
            else if (funct3 == 5)
            {
                unsigned funct7 = word >> 25;
                unsigned shift = (word >> 20) & 31;
                CHECK(funct7 == 0 || funct7 == 0x20 || funct7 == 0x24);
                regs[rd] = funct7 == 0x24 ? (regs[rs1] >> shift) & 1 :
                        funct7 ? (uint32_t)((int32_t)regs[rs1] >> shift) :
                        regs[rs1] >> shift;
            }
            else if (funct3 == 0)
                regs[rd] = regs[rs1] + ((int32_t)word >> 20);
            else if (funct3 == 2)
                regs[rd] = (int32_t)regs[rs1] < ((int32_t)word >> 20);
            else if (funct3 == 3)
                regs[rd] = regs[rs1] < (uint32_t)((int32_t)word >> 20);
            else if (funct3 == 4)
                regs[rd] = regs[rs1] ^ (uint32_t)((int32_t)word >> 20);
            else if (funct3 == 7)
                regs[rd] = regs[rs1] & (uint32_t)((int32_t)word >> 20);
            else
                CHECK(false);
        }
        else if (op == 0x03)
        {
            uint32_t address = regs[rs1] + ((int32_t)word >> 20);
            if (((word >> 12) & 7) == 4)
            {
                CHECK(address >= WAIT_ADDR && address < WAIT_ADDR + 32);
                regs[rd] = address == WAIT_ADDR ? wait :
                        ws_cyc_seq[(address - WAIT_ADDR) / 2][(address - WAIT_ADDR) & 1];
            }
            else
            {
                CHECK(((word >> 12) & 7) == 2);
                CHECK(run_code_base && address >= run_code_base &&
                        address - run_code_base < size && !(address & 3));
                regs[rd] = words[(address - run_code_base) / 4];
            }
        }
        else if (op == 0x33)
        {
            unsigned funct3 = (word >> 12) & 7;
            unsigned funct7 = (word >> 25) & 127;
            if (funct7 == 1 && funct3 == 0)
                regs[rd] = regs[rs1] * regs[rs2];
            else if (funct7 == 32 && funct3 == 0)
                regs[rd] = regs[rs1] - regs[rs2];
            else if (funct7 == 0 && funct3 == 0)
                regs[rd] = regs[rs1] + regs[rs2];
            else if (funct7 == 0 && funct3 == 2)
                regs[rd] = (int32_t)regs[rs1] < (int32_t)regs[rs2];
            else if (funct7 == 0 && funct3 == 3)
                regs[rd] = regs[rs1] < regs[rs2];
            else if (funct7 == 0 && funct3 == 4)
                regs[rd] = regs[rs1] ^ regs[rs2];
            else if (funct7 == 0 && funct3 == 7)
                regs[rd] = regs[rs1] & regs[rs2];
            else if (funct7 == 16 && (funct3 == 2 || funct3 == 4 || funct3 == 6))
                regs[rd] = (regs[rs1] << (funct3 / 2)) + regs[rs2];
            else
                CHECK(false);
        }
        else
            CHECK(false);
        at += length;
    }
    *remaining = regs[RV_BUDGET];
    *pc = regs[RV_PC];
    if (final)
        memcpy(final, regs, sizeof(regs));
    return RAN;

fallback:
    *remaining = regs[RV_BUDGET];
    *pc = regs[RV_PC];
    if (final)
        memcpy(final, regs, sizeof(regs));
    return FALLBACK;
}

static void test_batch(unsigned count, unsigned fast_instructions,
        unsigned saved_instructions)
{
    uint32_t words[64] = {0};
    uint8_t *ptr = (uint8_t *)words;
    uint8_t *limit = ptr + sizeof(words);
    uint8_t *slow_site = NULL;
    CHECK(emit_thumb_preflight_code(&ptr, limit, WAIT_ADDR, count,
            &slow_site));
    unsigned before_finish = (unsigned)(ptr - (uint8_t *)words) / 4;
    CHECK(gpsp_rv32_emit_thumb_batch_finish(&ptr, limit, count));
    unsigned length = (unsigned)(ptr - (uint8_t *)words) / 4;
    CHECK(length == before_finish + 1);
    /* Powers of two scale with one shift; Zba covers 3, 5 and 9. */
    bool one_word = !(count & (count - 1)) ||
            (GPSP_RV32_USE_ZBA && (count == 3 || count == 5 || count == 9));
    CHECK(length == (one_word ? 10u : 11u));

    size_t fallback_at = slow_site - (uint8_t *)words;
    CHECK(fallback_at < before_finish * 4);

    unsigned steps;
    uint32_t remaining, pc;
    for (unsigned wait = 3; wait <= 9; wait += 2)
    {
        CHECK(run(words, length * 4, fallback_at, wait, count * wait,
                &steps, &remaining, &pc, NULL, NULL) == RAN);
        CHECK(steps == fast_instructions);
        CHECK(remaining == 0);
        CHECK(pc == START_PC + count * 2);

        CHECK(run(words, length * 4, fallback_at, wait, count * wait - 1,
                &steps, &remaining, &pc, NULL, NULL) == FALLBACK);
        CHECK(remaining == count * wait - 1);
        CHECK(pc == START_PC);
    }
    CHECK(run(words, length * 4, fallback_at, 3, 0,
            &steps, &remaining, &pc, NULL, NULL) == RETURNED);
    CHECK(remaining == 0 && pc == START_PC);
    CHECK((3 + count * 5) - fast_instructions == saved_instructions);
}

static void test_invalid(void)
{
    uint32_t words[32] = {0};
    uint8_t *ptr = (uint8_t *)words;
    uint8_t *limit = ptr + sizeof(words);
    uint8_t *slow_site = NULL;
    CHECK(!emit_thumb_preflight_code(&ptr, limit, WAIT_ADDR, 0,
            &slow_site));
    CHECK(!emit_thumb_preflight_code(&ptr, limit, WAIT_ADDR, 33,
            &slow_site));
#if UINTPTR_MAX > UINT32_MAX
    CHECK(!emit_thumb_preflight_code(&ptr, limit,
            (uintptr_t)UINT32_MAX + 1, 8, &slow_site));
#endif
    CHECK(ptr == (uint8_t *)words);
    CHECK(slow_site == NULL);
}

static void test_slow_first_op(void)
{
    uint32_t words[256] = {0};
    uint8_t *ptr = (uint8_t *)words;
    uint8_t *limit = ptr + sizeof(words);
    unsigned steps;
    uint32_t remaining, pc;
    uint32_t initial[32] = {0};
    uint32_t final[32];

    CHECK(emit_thumb_batch_slow_code(&ptr, limit, START_PC,
            0x3501, WAIT_ADDR)); /* ADDS r5,#1 */
    CHECK((ptr - (uint8_t *)words) / 4 == 11);
    initial[guest_reg_map[5]] = 0x7fffffff;
    CHECK(run(words, 44, 44, 3, 2, &steps, &remaining, &pc,
            initial, final) == RETURNED);
    CHECK(steps == 11);
    CHECK(remaining == UINT32_MAX && pc == START_PC + 2);
    CHECK(final[guest_reg_map[5]] == 0x80000000);
    CHECK(final[RV_FLAG_N] == 1 && final[RV_FLAG_Z] == 0 &&
            final[RV_FLAG_C] == 0 && final[RV_FLAG_V] == 1);

    memset(words, 0, sizeof(words));
    ptr = (uint8_t *)words;
    CHECK(emit_thumb_batch_slow_code(&ptr, limit, START_PC,
            0x3501, WAIT_ADDR));
    initial[guest_reg_map[5]] = UINT32_MAX;
    CHECK(run(words, 44, 44, 3, 2, &steps, &remaining, &pc,
            initial, final) == RETURNED);
    CHECK(final[guest_reg_map[5]] == 0 && final[RV_FLAG_N] == 0 &&
            final[RV_FLAG_Z] == 1 && final[RV_FLAG_C] == 1 && final[RV_FLAG_V] == 0);

    memset(words, 0, sizeof(words));
    ptr = (uint8_t *)words;
    CHECK(emit_thumb_batch_slow_code(&ptr, limit, START_PC,
            0x0001, WAIT_ADDR)); /* LSLS r1,r0,#0 */
    CHECK((ptr - (uint8_t *)words) / 4 == 8);
    memset(initial, 0, sizeof(initial));
    initial[guest_reg_map[0]] = 0x80000000;
    initial[RV_FLAG_C] = initial[RV_FLAG_V] = 1;
    CHECK(run(words, 32, 32, 3, 2, &steps, &remaining, &pc,
            initial, final) == RETURNED);
    CHECK(final[guest_reg_map[1]] == 0x80000000 && final[RV_FLAG_N] == 1 &&
            final[RV_FLAG_Z] == 0 && final[RV_FLAG_C] == 1 && final[RV_FLAG_V] == 1);
}

static void test_slow_pc_independence(void)
{
    enum { OTHER_PC = START_PC + 0x102 };
    CHECK(gpsp_rv32_thumb_batch_pc_sensitive(0x4400));
    CHECK(gpsp_rv32_thumb_batch_pc_sensitive(0xa000));
    CHECK(!gpsp_rv32_thumb_batch_pc_sensitive(0x3501));

    for (unsigned opcode = 0; opcode <= UINT16_MAX; opcode++)
    {
        if (!gpsp_rv32_thumb_batch_alu((uint16_t)opcode) ||
            gpsp_rv32_thumb_batch_pc_sensitive((uint16_t)opcode))
            continue;

        uint32_t first[256], other[256];
        uint8_t *first_ptr = (uint8_t *)first;
        uint8_t *other_ptr = (uint8_t *)other;
        CHECK(emit_thumb_batch_slow_code(&first_ptr,
                (uint8_t *)first + sizeof(first), START_PC,
                (uint16_t)opcode, WAIT_ADDR));
        CHECK(emit_thumb_batch_slow_code(&other_ptr,
                (uint8_t *)other + sizeof(other), OTHER_PC,
                (uint16_t)opcode, WAIT_ADDR));
        size_t size = (size_t)(first_ptr - (uint8_t *)first);
        CHECK(size == (size_t)(other_ptr - (uint8_t *)other));
        CHECK(memcmp(first, other, size) == 0);
    }
}

/* gpsp_rv32_thumb_side_preflight_stub for the current register map. These
 * words are checked against the assembled stub by test-side-stub. */
static uint32_t side_stub_words[16];

static void build_side_stub(void)
{
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, side_stub_words, sizeof(side_stub_words));
    code.compress = false;
    CHECK(gpsp_rv32_blt(&code, X0, RV_BUDGET, 12) &&
            gpsp_rv32_addi(&code, A1, RV_TMP1, -12) &&
            gpsp_rv32_jalr(&code, X0, RA, 4) &&
            gpsp_rv32_srli(&code, A1, A0, 5) &&
            gpsp_rv32_andi(&code, A0, A0, 31) &&
            gpsp_rv32_lw(&code, RV_TMP0, RV_TMP1, 8) &&
            gpsp_rv32_lbu(&code, RV_TMP0, RV_TMP0, 0) &&
            gpsp_rv32_mul(&code, A1, A1, RV_TMP0) &&
            gpsp_rv32_bge(&code, RV_BUDGET, A1, 12) &&
            gpsp_rv32_lw(&code, RV_TMP0, RV_TMP1, 4) &&
            gpsp_rv32_jalr(&code, X0, RV_TMP0, 0) &&
            gpsp_rv32_sub(&code, RV_BUDGET, RV_BUDGET, A1) &&
            gpsp_rv32_slli(&code, A0, A0, 1) &&
            gpsp_rv32_sub(&code, RV_PC, RV_PC, A0) &&
            gpsp_rv32_lw(&code, RV_TMP0, RV_TMP1, 0) &&
            gpsp_rv32_jalr(&code, X0, RV_TMP0, 0));
    CHECK(code.ptr == code.end);
    static const uint32_t expected[] = {
        0x00904663, 0xff478593, 0x00408067, 0x00555593,
        0x01f57513, 0x0087a703, 0x00074703, 0x02e585b3,
        0x00b4d663, 0x0047a703, 0x00070067, 0x40b484b3,
        0x00151513, 0x40a90933, 0x0007a703, 0x00070067,
    };
    CHECK(memcmp(side_stub_words, expected, sizeof(expected)) == 0);
}

/* Instructions emitted, and those outside a fixed span. */
static unsigned hook_words, free_words;

static void count_free(const gpsp_rv32_code *code)
{
    hook_words++;
    free_words += !code->fixed_depth;
}

static void test_side_entry_at(uint32_t entry_pc, unsigned wait_region)
{
    enum { BODY_WORD = 80, CODE_BASE = 0x30000000,
            STUB_BASE = 0x30001000 };
    uint32_t words[256] = {0};
    uint8_t *ptr = (uint8_t *)words;
    uint8_t *limit = ptr + sizeof(words);
    uint8_t *slow_literal = NULL;
    uint8_t *body_address = (uint8_t *)(uintptr_t)(CODE_BASE + BODY_WORD * 4);
    uint32_t wait_address = WAIT_ADDR + wait_region * 2;

    /* The stub finds its data and retry point by distance. */
    gpsp_rv32_test_emit_hook = count_free;
    hook_words = free_words = 0;
    CHECK(gpsp_rv32_emit_thumb_side_entry(&ptr, limit, entry_pc,
            0x3501, wait_address, 1, 31, body_address, STUB_BASE,
            &slow_literal));
    gpsp_rv32_test_emit_hook = NULL;
    CHECK(hook_words == 3 && free_words == 0);
    CHECK(ptr == (uint8_t *)words + 24);
    CHECK(slow_literal == (uint8_t *)&words[4]);
    CHECK(words[3] == CODE_BASE + BODY_WORD * 4);
    CHECK(words[5] == wait_address);
    unsigned slow_word = (unsigned)(ptr - (uint8_t *)words) / 4;
    CHECK(emit_thumb_batch_slow_code(&ptr, limit, entry_pc,
            0x3501, wait_address));
    uint32_t slow_target = CODE_BASE + slow_word * 4;
    memcpy(slow_literal, &slow_target, sizeof(slow_target));
    CHECK((unsigned)(ptr - (uint8_t *)words) / 4 < BODY_WORD);

    gpsp_rv32_code body;
    gpsp_rv32_code_init(&body, &words[BODY_WORD],
            sizeof(words) - BODY_WORD * 4);
    CHECK(gpsp_rv32_emit_thumb(&body, 0x3501, entry_pc, 0xF) ==
            GPSP_RV32_THUMB_EMITTED);
    for (unsigned i = 1; i < 31; i++)
        CHECK(gpsp_rv32_addi(&body, X0, X0, 0));
    uint8_t *finish = body.ptr;
    CHECK(gpsp_rv32_emit_thumb_batch_finish(&finish, body.end, 32));
    body.ptr = finish;
    CHECK(emit_return(&body));

    uint32_t initial[32] = {0}, final[32];
    initial[RV_PC] = entry_pc;
    initial[guest_reg_map[5]] = 0x7fffffff;
    unsigned steps;
    uint32_t remaining, pc;
    run_code_base = CODE_BASE;
    run_stub_base = STUB_BASE;
    run_stub_words = side_stub_words;
    run_stub_size = sizeof(side_stub_words);
    size_t size = body.ptr - (uint8_t *)words;
    ws_cyc_seq[wait_region][0] = 3;
    CHECK(run(words, size, size, 3, 93, &steps,
            &remaining, &pc, initial, final) == RETURNED);
    CHECK(remaining == 0 && pc == entry_pc - 2 + 64);
    CHECK(final[guest_reg_map[5]] == 0x80000000);
    CHECK(final[RV_FLAG_N] == 1 && final[RV_FLAG_Z] == 0 &&
            final[RV_FLAG_C] == 0 && final[RV_FLAG_V] == 1);

    ws_cyc_seq[wait_region][0] = 5;
    CHECK(run(words, size, size, 0, 155, &steps,
            &remaining, &pc, initial, final) == RETURNED);
    CHECK(remaining == 0 && pc == entry_pc - 2 + 64);

    ws_cyc_seq[wait_region][0] = 3;
    CHECK(run(words, size, size, 3, 2, &steps,
            &remaining, &pc, initial, final) == RETURNED);
    CHECK(remaining == UINT32_MAX && pc == entry_pc + 2);
    CHECK(final[guest_reg_map[5]] == 0x80000000);
    CHECK(final[RV_FLAG_N] == 1 && final[RV_FLAG_Z] == 0 &&
            final[RV_FLAG_C] == 0 && final[RV_FLAG_V] == 1);

    initial[RA] = 0x40000000;
    CHECK(run(words, size, size, 3, 0, &steps,
            &remaining, &pc, initial, final) == RETURNED);
    CHECK(remaining == 0 && pc == entry_pc);
    CHECK(final[A1] == CODE_BASE && run_last_target == initial[RA] + 4);
    CHECK(final[guest_reg_map[5]] == initial[guest_reg_map[5]]);
    run_code_base = 0;
    run_stub_base = 0;
    run_stub_words = NULL;
    run_stub_size = 0;

    ptr = (uint8_t *)words;
    slow_literal = NULL;
    CHECK(!gpsp_rv32_emit_thumb_side_entry(&ptr, ptr + 20, entry_pc,
            0x3501, wait_address, 1, 31, body_address, STUB_BASE,
            &slow_literal));
    CHECK(ptr == (uint8_t *)words);
}

static void test_side_entry(void)
{
    test_side_entry_at(START_PC + 2, 8);
    ws_cyc_seq[8][0] = 1;
    test_side_entry_at(0x08fffffe, 9);
}

static void test_side_stub_binary(const char *path)
{
    FILE *file = fopen(path, "rb");
    CHECK(file);
    uint32_t actual[sizeof(side_stub_words) / sizeof(side_stub_words[0])];
    CHECK(fread(actual, 1, sizeof(actual), file) == sizeof(actual));
    CHECK(fgetc(file) == EOF);
    CHECK(fclose(file) == 0);
    CHECK(memcmp(actual, side_stub_words, sizeof(actual)) == 0);
}

static void test_fast_bytes_unchanged(void)
{
    uint32_t previous[32] = {0};
    uint32_t current[32] = {0};
    gpsp_rv32_code old;
    gpsp_rv32_code_init(&old, previous, sizeof(previous));
    CHECK(emit_guard(&old));
    CHECK(emit_absolute_load(&old, RV_TMP0, RV_TMP0, WAIT_ADDR, false));
    CHECK(emit_scaled_wait(&old, 5));
    uint8_t *branch = old.ptr;
    CHECK(gpsp_rv32_bge(&old, RV_BUDGET, RV_TMP0, 0));
    CHECK(gpsp_rv32_emit_interpreter_step_stub(&old, 0x30002004) ==
            GPSP_RV32_FALLBACK_EMITTED);
    CHECK(gpsp_rv32_patch_branch(&old, branch, old.ptr));
    CHECK(gpsp_rv32_sub(&old, RV_BUDGET, RV_BUDGET, RV_TMP0));

    uint8_t *ptr = (uint8_t *)current;
    uint8_t *site = NULL;
    CHECK(emit_thumb_preflight_code(&ptr, ptr + sizeof(current),
            WAIT_ADDR, 5, &site));
    size_t size = (size_t)(ptr - (uint8_t *)current);
    size_t slot_offset = (size_t)(site - (uint8_t *)current);
    CHECK(size == (size_t)(old.ptr - old.start));
    CHECK(memcmp(current, previous, slot_offset) == 0);
    CHECK(memcmp((uint8_t *)current + slot_offset + 8,
            (uint8_t *)previous + slot_offset + 8,
            size - slot_offset - 8) == 0);

    gpsp_rv32_code patched;
    gpsp_rv32_code_init(&patched, current, sizeof(current));
    patched.ptr = ptr;
    CHECK(gpsp_rv32_patch_link_slot(&patched, site, 0x30002000));
    CHECK(memcmp((uint8_t *)current + slot_offset + 8,
            (uint8_t *)previous + slot_offset + 8,
            size - slot_offset - 8) == 0);
}

#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
static void test_byte_stats(void)
{
    uint32_t words[512] = {0};
    uint8_t *ptr = (uint8_t *)words;
    uint8_t *limit = ptr + sizeof(words);
    uint8_t *slow_site = NULL;
    struct gpsp_rv32_emit_byte_stats before, after;

    gpsp_rv32_emit_get_byte_stats(&before);
    CHECK(emit_thumb_preflight_code(&ptr, limit, WAIT_ADDR, 3,
            &slow_site));
    CHECK(gpsp_rv32_emit_thumb_batch_finish(&ptr, limit, 3));
    size_t preflight = (size_t)(ptr - (uint8_t *)words);
    CHECK(emit_thumb_batch_slow_code(&ptr, limit, START_PC,
            0x3501, WAIT_ADDR));
    size_t slow = (size_t)(ptr - (uint8_t *)words) - preflight;
    CHECK(gpsp_rv32_emit_block_return(&ptr, limit));
    gpsp_rv32_emit_get_byte_stats(&after);

    CHECK(after.batch_preflight - before.batch_preflight == preflight);
    CHECK(after.batch_slow - before.batch_slow == slow);
    CHECK(after.link_exit - before.link_exit == 4);
    CHECK(after.guard == before.guard);
    CHECK(after.body == before.body);
    CHECK(after.timing == before.timing);
}
#endif

static void test_folded_wait_offsets(void)
{
    uint32_t words[4] = {0};
    gpsp_rv32_code code;

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_absolute_load(&code, T6, T5, 0x200007ff, false));
    CHECK((code.ptr - code.start) / 4 == 2);
    CHECK((words[0] & 0xfffff000) == 0x20000000);
    CHECK((words[1] & 0x7f) == 0x03);
    CHECK((int32_t)words[1] >> 20 == 2047);

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_absolute_load(&code, T6, T5, 0x20000800, false));
    CHECK((code.ptr - code.start) / 4 == 2);
    CHECK((words[0] & 0xfffff000) == 0x20001000);
    CHECK((int32_t)words[1] >> 20 == -2048);

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_absolute_load(&code, T6, T5, 0x000007ff, false));
    CHECK((code.ptr - code.start) / 4 == 1);
    CHECK(((words[0] >> 15) & 31) == X0);
    CHECK((int32_t)words[0] >> 20 == 2047);

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_indexed_wait_load(&code, 0x20000800));
    CHECK((code.ptr - code.start) / 4 == 3);
    CHECK((words[0] & 0xfffff000) == 0x20001000);
    CHECK((words[1] & 0x7f) == 0x33);
    CHECK((int32_t)words[2] >> 20 == -2048);

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_absolute_load(&code, T5, T5, 0x20000800, true));
    CHECK((code.ptr - code.start) / 4 == 2);
    CHECK(((words[1] >> 12) & 7) == 2);
}

static void test_boundaries(void)
{
    CHECK(gpsp_rv32_thumb_batch_alu(0x2001)); /* MOVS r0,#1 */
    CHECK(gpsp_rv32_thumb_batch_alu(0xA000)); /* ADD r0,PC,#0 */
    CHECK(gpsp_rv32_thumb_batch_alu(0xB000)); /* ADD SP,#0 */
    CHECK(!gpsp_rv32_thumb_batch_alu(0x6800)); /* LDR */
    CHECK(!gpsp_rv32_thumb_batch_alu(0x6000)); /* STR */
    CHECK(!gpsp_rv32_thumb_batch_alu(0xB400)); /* PUSH */
    CHECK(!gpsp_rv32_thumb_batch_alu(0x4080)); /* register LSL */
    CHECK(!gpsp_rv32_thumb_batch_alu(0x4340)); /* MUL */
    CHECK(!gpsp_rv32_thumb_batch_alu(0x4487)); /* ADD PC */
    CHECK(!gpsp_rv32_thumb_batch_alu(0x4687)); /* MOV PC */
    CHECK(!gpsp_rv32_thumb_batch_alu(0x4700)); /* BX */
    CHECK(!gpsp_rv32_thumb_batch_alu(0xD100)); /* BNE */
    CHECK(!gpsp_rv32_thumb_batch_alu(0xE000)); /* B */
    CHECK(!gpsp_rv32_thumb_batch_alu(0xF000)); /* BL first */
    CHECK(!gpsp_rv32_thumb_batch_alu(0xF800)); /* BL second */
}

static void test_fixed_fetch_wait(void)
{
    uint32_t words[32] = {0};
    gpsp_rv32_code code;
    unsigned steps;
    uint32_t remaining, pc;

    ws_cyc_seq[2][0] = 3;
    ws_cyc_seq[2][1] = ws_cyc_nseq[2][1] = 6;
    ws_cyc_seq[3][0] = 1;

    /* The next fetch crosses from EWRAM into IWRAM. */
    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_advance(&code, 0x02fffffe, true));
    CHECK((code.ptr - code.start) / 4 == 2);
    CHECK(run(words, 8, 8, 0, 10, &steps, &remaining, &pc,
            NULL, NULL) == RAN);
    CHECK(remaining == 9 && pc == START_PC + 2);

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_advance(&code, 0x01fffffc, false));
    CHECK((code.ptr - code.start) / 4 == 2);
    CHECK(run(words, 8, 8, 0, 10, &steps, &remaining, &pc,
            NULL, NULL) == RAN);
    CHECK(remaining == 4 && pc == START_PC + 4);

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_advance(&code, 0x00000100, true));
    CHECK((code.ptr - code.start) / 4 == 2);
    CHECK(run(words, 8, 8, 0, 10, &steps, &remaining, &pc,
            NULL, NULL) == RAN);
    CHECK(remaining == 9 && pc == START_PC + 2);

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_advance_wait(&code, START_PC, true, WAIT_ADDR));
    CHECK((code.ptr - code.start) / 4 == 4);
    CHECK(run(words, 16, 16, 3, 10, &steps, &remaining, &pc,
            NULL, NULL) == RAN);
    CHECK(remaining == 7 && pc == START_PC + 2);

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_branch_advance(&code, START_PC, 0x02000100, false));
    CHECK((code.ptr - code.start) / 4 == 3);
    CHECK(run(words, 12, 12, 0, 20, &steps, &remaining, &pc,
            NULL, NULL) == RAN);
    CHECK(remaining == 8 && pc == 0x02000100);

    uint8_t *ptr = (uint8_t *)words;
    uint8_t *slow_site = NULL;
    CHECK(emit_thumb_preflight_code(&ptr, ptr + sizeof(words),
            (uintptr_t)&ws_cyc_seq[2][0], 3, &slow_site));
    CHECK((ptr - (uint8_t *)words) / 4 == 7);
    CHECK(gpsp_rv32_emit_thumb_batch_finish(&ptr,
            (uint8_t *)words + sizeof(words), 3));
    CHECK((ptr - (uint8_t *)words) / 4 == 8);
    CHECK(run(words, 32, slow_site - (uint8_t *)words, 0, 9, &steps,
            &remaining, &pc, NULL, NULL) == RAN);
    CHECK(steps == 5 && remaining == 0 && pc == START_PC + 6);
}

/* Mixed 4-byte and 2-byte instructions run by byte offset. */
static void test_mixed_lengths(void)
{
    enum { CODE_BASE = 0x30000000 };
    uint8_t code[12];
    uint32_t words[3] = {0};
    uint32_t addi = 0x00150513;      /* addi a0, a0, 1 */
    uint16_t c_addi = 0x0505;        /* c.addi a0, 1 */
    uint16_t c_jalr = 0x9782;        /* c.jalr a5 */
    memcpy(code, &addi, 4);
    memcpy(code + 4, &c_addi, 2);
    memcpy(code + 6, &c_jalr, 2);
    memcpy(words, code, 8);

    uint32_t initial[32] = {0}, final[32];
    unsigned steps;
    uint32_t remaining, pc;
    CHECK(run(words, 6, 6, 0, 0, &steps, &remaining, &pc,
            initial, final) == RAN);
    CHECK(steps == 2 && final[A0] == 2);

    run_code_base = CODE_BASE;
    initial[A5] = 0x40000000;
    CHECK(run(words, 8, 8, 0, 0, &steps, &remaining, &pc,
            initial, final) == RETURNED);
    CHECK(steps == 3 && final[A0] == 2);
    CHECK(final[RA] == CODE_BASE + 8 && run_last_target == initial[A5]);
    run_code_base = 0;
}

static void test_generated_jalr(void)
{
    static const unsigned sources[] = {A5, RA};
    enum { CODE_BASE = 0x30000000, TARGET = 0x40001001 };
    run_code_base = CODE_BASE;
    for (unsigned compress = 0; compress <= 1; compress++)
        for (unsigned i = 0; i < sizeof(sources) / sizeof(sources[0]); i++)
        {
            uint32_t words[4] = {0};
            gpsp_rv32_code code;
            gpsp_rv32_code_init(&code, words, sizeof(words));
            code.compress = compress;
            CHECK(gpsp_rv32_addi(&code, A0, X0, 1));
            size_t call_offset = code.ptr - code.start;
            CHECK(call_offset == (compress ? 2u : 4u));
            CHECK(gpsp_rv32_jalr(&code, RA, sources[i], 0));
            size_t size = code.ptr - code.start;
            CHECK(size - call_offset == (compress ? 2u : 4u));

            uint32_t initial[32] = {0}, final[32];
            initial[sources[i]] = TARGET;
            unsigned steps;
            uint32_t remaining, pc;
            CHECK(run(words, size, size, 0, 0, &steps, &remaining, &pc,
                    initial, final) == RETURNED);
            CHECK(steps == 2 && final[A0] == 1);
            CHECK(final[RA] == CODE_BASE + (compress ? 4u : 8u));
            /* The target uses the old source value, even when it is ra. */
            CHECK(run_last_target == (TARGET & ~1u));
        }
    run_code_base = 0;
}

static void test_thumb_bcc_paths(void)
{
    const uint32_t pc = 0x02fffffa;
    const uint32_t target = pc + 8;
    uint32_t words[32] = {0};
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    gpsp_rv32_arm_condition_skip skip = { { NULL, NULL }, 0 };
    CHECK(gpsp_rv32_emit_arm_condition_skip(&code, 0, &skip)); /* BEQ */
    CHECK(emit_thumb_bcc_advance(&code, pc, target));
    uint8_t *slot = NULL;
    CHECK(gpsp_rv32_emit_link_slot(&code, &slot));
    CHECK(gpsp_rv32_patch_arm_condition_skip(&code, &skip, code.ptr));
    CHECK(emit_thumb_bcc_advance(&code, pc, pc + 2));
    CHECK(emit_return(&code));
    CHECK((code.ptr - code.start) == 32);
    CHECK((slot - code.start) == 12);

    uint32_t initial[32] = {0}, final[32];
    unsigned steps;
    uint32_t remaining, next_pc;
    initial[RV_PC] = pc;
    initial[RV_FLAG_Z] = 1;
    CHECK(run(words, 32, 32, 0, 20, &steps, &remaining, &next_pc,
            initial, final) == RETURNED);
    CHECK(remaining == 18 && next_pc == target);
    CHECK(final[RV_FLAG_Z] == 1);

    initial[RV_FLAG_Z] = 0;
    CHECK(run(words, 32, 32, 0, 20, &steps, &remaining, &next_pc,
            initial, final) == RETURNED);
    CHECK(remaining == 14 && next_pc == pc + 2);
    CHECK(final[RV_FLAG_Z] == 0);
}

static void test_thumb_bcc_shared_wait_at(uint32_t instruction_pc,
        uint32_t target, bool shared_wait, uint32_t taken_cycles,
        uint32_t fallthrough_cycles)
{
    uint32_t words[32] = {0};
    uint8_t *ptr = (uint8_t *)words;
    uint8_t *limit = ptr + sizeof(words);
    bool ends_block = true;
    gpsp_rv32_block_link link = {0};
    CHECK(gpsp_rv32_emit_block_instruction(&ptr, limit, instruction_pc,
            0xd002, true, true, 0xf, false, &ends_block, &link)); /* BEQ +8 */
    CHECK(!ends_block && link.site && link.thumb && link.target == target);
    CHECK((size_t)(ptr - (uint8_t *)words) == (shared_wait ? 32u : 36u));
    CHECK(gpsp_rv32_emit_block_return(&ptr, limit));
    size_t size = ptr - (uint8_t *)words;

    uint32_t initial[32] = {0}, final[32];
    unsigned steps;
    uint32_t remaining, pc;
    initial[RV_PC] = instruction_pc;
    initial[RV_FLAG_Z] = 1;
    CHECK(run(words, size, size, 0, 20, &steps,
            &remaining, &pc, initial, final) == RETURNED);
    CHECK(remaining == 20 - taken_cycles && pc == target);
    CHECK(final[RV_FLAG_Z] == 1);

    initial[RV_FLAG_Z] = 0;
    CHECK(run(words, size, size, 0, 20, &steps,
            &remaining, &pc, initial, final) == RETURNED);
    CHECK(remaining == 20 - fallthrough_cycles && pc == instruction_pc + 2);
    CHECK(final[RV_FLAG_Z] == 0);
}

static void test_thumb_bcc_shared_wait(void)
{
    test_thumb_bcc_shared_wait_at(0x02000100, 0x02000108,
            true, 6, 6);
    /* The taken target enters IWRAM while fallthrough stays in EWRAM. */
    test_thumb_bcc_shared_wait_at(0x02fffffa, 0x03000002,
            false, 2, 6);
}

static void test_rom_literal_address(void)
{
    static const struct
    {
        uint32_t pc;
        uint16_t opcode;
        uint32_t rom_size;
        bool folded;
        uint32_t address;
    } cases[] = {
        {0x08000100, 0x4800, 0x8000, true, 0x08000104},
        {0x08007bfc, 0x48ff, 0x8000, true, 0x08007ffc},
        {0x08007c00, 0x48ff, 0x10000, false, 0},
        {0x08007ffe, 0x4800, 0x10000, false, 0},
        {0x08000100, 0x4800, 0x108, true, 0x08000104},
        {0x08000100, 0x4800, 0x107, false, 0},
        {0x080000bc, 0x4800, 0x8000, true, 0x080000c0},
        {0x080000c0, 0x4800, 0x8000, false, 0},
        {0x080000c4, 0x4800, 0x8000, false, 0},
        {0x080000c8, 0x4800, 0x8000, true, 0x080000cc},
        {0x0a0000c0, 0x4800, 0x8000, false, 0},
        {0x0d000100, 0x4800, 0x8000, false, 0},
        {0x08000100, 0x4700, 0x8000, false, 0},
        {0x08000101, 0x4800, 0x8000, false, 0},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        uint32_t address = UINT32_MAX;
        CHECK(gpsp_rv32_thumb_rom_literal_address(cases[i].pc,
                cases[i].opcode, cases[i].rom_size, &address) == cases[i].folded);
        CHECK(address == (cases[i].folded ? cases[i].address : UINT32_MAX));
    }
    CHECK(!gpsp_rv32_thumb_rom_literal_address(START_PC, 0x4800,
            0x8000, NULL));
}

static void test_rom_literal_constant(void)
{
    uint32_t words[32] = {0};
    uint8_t *ptr = (uint8_t *)words;
    uint8_t *limit = ptr + sizeof(words);
    CHECK(emit_thumb_literal_constant_wait(&ptr, limit, START_PC,
            0x4b00, 0x12345678, WAIT_ADDR + 2, WAIT_ADDR));
    CHECK(ptr == (uint8_t *)words + 44);

    uint32_t initial[32] = {0}, final[32];
    unsigned steps;
    uint32_t remaining, pc;
    ws_cyc_seq[1][0] = 7;
    CHECK(run(words, 44, 44, 3, 20, &steps, &remaining, &pc,
            initial, final) == RAN);
    CHECK(final[guest_reg_map[3]] == 0x12345678);
    CHECK(remaining == 10 && pc == START_PC + 2);

    ws_cyc_seq[1][0] = 5;
    CHECK(run(words, 44, 44, 9, 20, &steps, &remaining, &pc,
            initial, final) == RAN);
    CHECK(remaining == 6 && pc == START_PC + 2);

    initial[RA] = 0x40000000;
    CHECK(run(words, 44, 44, 3, 0, &steps, &remaining, &pc,
            initial, final) == RETURNED);
    CHECK(remaining == 0 && pc == START_PC);
    CHECK(final[guest_reg_map[3]] == 0);
    CHECK(run_last_target == initial[RA] + 4);

    ptr = (uint8_t *)words;
    CHECK(!emit_thumb_literal_constant_wait(&ptr, limit, START_PC,
            0x4800, 1, WAIT_ADDR, 0));
    CHECK(ptr == (uint8_t *)words);
}

int main(int argc, char **argv)
{
    CHECK(argc <= 2);
    /* Exact sizes below assume 32-bit instructions. */
    gpsp_rv32_test_compress = false;
    build_side_stub();
    if (argc == 2)
        test_side_stub_binary(argv[1]);
    test_invalid();
    test_boundaries();
    test_slow_first_op();
    test_slow_pc_independence();
    test_side_entry();
    test_fast_bytes_unchanged();
#if defined(GPSP_BENCHMARK_JIT_CODE_BYTES)
    test_byte_stats();
#endif
    test_folded_wait_offsets();
    test_fixed_fetch_wait();
    test_mixed_lengths();
    test_generated_jalr();
    test_thumb_bcc_paths();
    test_thumb_bcc_shared_wait();
    test_rom_literal_address();
    test_rom_literal_constant();
#if GPSP_RV32_USE_ZBA
    test_batch(3, 7, 11);
    test_batch(5, 7, 21);
#else
    test_batch(3, 8, 10);
    test_batch(5, 8, 20);
#endif
    test_batch(8, 7, 36);
    test_batch(16, 7, 76);
    test_batch(32, 7, 156);
    puts("rv32-thumb-batch: ok");
    return 0;
}
