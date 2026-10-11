/* gameplaySP
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_arm_alu.h"
#include "rv32_register_map.h"

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "../rv32-rvc.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition); \
        exit(1); \
    } \
} while (0)

static uint32_t read_word(const uint8_t *ptr)
{
    uint32_t word;
    memcpy(&word, ptr, sizeof(word));
    return word;
}

static int32_t branch_offset(uint32_t word)
{
    uint32_t imm = ((word >> 31) << 12) | (((word >> 7) & 1) << 11) |
            (((word >> 25) & 0x3f) << 5) | (((word >> 8) & 15) << 1);
    if (imm & 0x1000)
        imm |= ~0x1fffU;
    return (int32_t)imm;
}

static int32_t jump_offset(uint32_t word)
{
    uint32_t imm = ((word >> 31) << 20) | (((word >> 12) & 0xff) << 12) |
            (((word >> 20) & 1) << 11) | (((word >> 21) & 0x3ff) << 1);
    if (imm & 0x100000)
        imm |= ~0x1fffffU;
    return (int32_t)imm;
}

static size_t emit_one(uint32_t opcode, uint32_t pc, uint32_t *storage);
static uint32_t rotate_right(uint32_t value, unsigned amount);

/* Execute the RV32 instructions used by this emitter.
 * The expected ARM results below use a separate arithmetic reference. */
static void execute_emitted(const uint32_t *code, size_t size, uint32_t host[32])
{
    size_t pc = 0;
    unsigned steps = 0;
    while (pc < size)
    {
        CHECK(++steps <= 128 && pc % 2 == 0);
        uint32_t word = 0;
        unsigned length = rv32_rvc_fetch(code, size, pc, &word);
        CHECK(length);
        if (!length)
            return;
        unsigned opcode = word & 0x7f;
        unsigned rd = (word >> 7) & 31;
        unsigned funct3 = (word >> 12) & 7;
        unsigned rs1 = (word >> 15) & 31;
        unsigned rs2 = (word >> 20) & 31;
        unsigned funct7 = word >> 25;
        int immediate = (int)(word >> 20);
        if (immediate & 0x800)
            immediate -= 4096;
        size_t next = pc + length;

        switch (opcode)
        {
            case 0x13: /* I-type */
                switch (funct3)
                {
                    case 0: host[rd] = host[rs1] + (uint32_t)immediate; break;
                    case 1:
                        CHECK(funct7 == 0);
                        host[rd] = host[rs1] << rs2;
                        break;
                    case 3: host[rd] = host[rs1] < (uint32_t)immediate; break;
                    case 4: host[rd] = host[rs1] ^ (uint32_t)immediate; break;
                    case 5:
                        CHECK(funct7 == 0 || funct7 == 0x20 || funct7 == 0x24 ||
                                funct7 == 0x30);
                        host[rd] = funct7 == 0x30 ?
                                rotate_right(host[rs1], rs2) :
                                funct7 == 0x24 ? (host[rs1] >> rs2) & 1 :
                                funct7 ? (uint32_t)((int32_t)host[rs1] >> rs2) :
                                host[rs1] >> rs2;
                        break;
                    case 6: host[rd] = host[rs1] | (uint32_t)immediate; break;
                    case 7: host[rd] = host[rs1] & (uint32_t)immediate; break;
                    default: CHECK(0);
                }
                break;
            case 0x33: /* R-type */
                switch (funct3)
                {
                    case 0:
                        CHECK(funct7 == 0 || funct7 == 0x20);
                        host[rd] = funct7 ? host[rs1] - host[rs2] :
                                host[rs1] + host[rs2];
                        break;
                    case 1: host[rd] = host[rs1] << (host[rs2] & 31); break;
                    case 2:
                        CHECK(funct7 == 0 || funct7 == 0x10);
                        host[rd] = funct7 ? (host[rs1] << 1) + host[rs2] :
                                (int32_t)host[rs1] < (int32_t)host[rs2];
                        break;
                    case 3: host[rd] = host[rs1] < host[rs2]; break;
                    case 4:
                        CHECK(funct7 == 0 || funct7 == 0x10 || funct7 == 0x20);
                        host[rd] = funct7 == 0x10 ? (host[rs1] << 2) + host[rs2] :
                                host[rs1] ^ (funct7 ? ~host[rs2] : host[rs2]);
                        break;
                    case 5:
                        CHECK(funct7 == 0 || funct7 == 0x20 || funct7 == 0x24 ||
                                funct7 == 0x30);
                        host[rd] = funct7 == 0x30 ?
                                rotate_right(host[rs1], host[rs2] & 31) :
                                funct7 == 0x24 ? (host[rs1] >> (host[rs2] & 31)) & 1 :
                                funct7 ? (uint32_t)((int32_t)host[rs1] >> (host[rs2] & 31)) :
                                host[rs1] >> (host[rs2] & 31);
                        break;
                    case 6:
                        CHECK(funct7 == 0 || funct7 == 0x10);
                        host[rd] = funct7 ? (host[rs1] << 3) + host[rs2] :
                                host[rs1] | host[rs2];
                        break;
                    case 7:
                        CHECK(funct7 == 0 || funct7 == 0x20);
                        host[rd] = host[rs1] &
                                (funct7 ? ~host[rs2] : host[rs2]);
                        break;
                    default: CHECK(0);
                }
                break;
            case 0x37: host[rd] = word & 0xfffff000; break; /* LUI */
            case 0x63: /* conditional skip */
            {
                CHECK(funct3 == 0 || funct3 == 1);
                if ((funct3 == 0 && host[rs1] == host[rs2]) ||
                    (funct3 == 1 && host[rs1] != host[rs2]))
                {
                    int64_t target = (int64_t)pc + branch_offset(word);
                    CHECK(target >= 0 && target <= (int64_t)size);
                    next = (size_t)target;
                }
                break;
            }
            case 0x6f: /* internal shifter jump */
            {
                int64_t target = (int64_t)pc + jump_offset(word);
                CHECK(target >= 0 && target <= (int64_t)size);
                host[rd] = (uint32_t)next;
                next = (size_t)target;
                break;
            }
            default: CHECK(0);
        }
        host[0] = 0;
        pc = next;
    }
    CHECK(pc == size);
}

#define mapped_reg guest_reg_map

static bool arm_condition(unsigned condition, unsigned n, unsigned z,
        unsigned c, unsigned v)
{
    switch (condition)
    {
        case 0: return z;
        case 1: return !z;
        case 2: return c;
        case 3: return !c;
        case 4: return n;
        case 5: return !n;
        case 6: return v;
        case 7: return !v;
        case 8: return c && !z;
        case 9: return !c || z;
        case 10: return n == v;
        case 11: return n != v;
        case 12: return !z && n == v;
        case 13: return z || n != v;
        case 14: return true;
        default: return false;
    }
}

typedef struct
{
    uint32_t reg[15];
    uint32_t n, z, c, v;
} arm_state;

static uint32_t arm_reg(const arm_state *state, unsigned reg, uint32_t pc,
        unsigned pc_offset)
{
    return reg == 15 ? pc + pc_offset : state->reg[reg];
}

static uint32_t rotate_right(uint32_t value, unsigned amount)
{
    return amount ? (value >> amount) | (value << (32 - amount)) : value;
}

static uint32_t arithmetic_right(uint32_t value, unsigned amount)
{
    if (amount >= 32)
        return (value >> 31) ? UINT32_MAX : 0;
    if (!amount)
        return value;
    return (value >> amount) |
            ((value >> 31) ? ~(UINT32_MAX >> amount) : 0);
}

static uint32_t reference_operand2(uint32_t opcode, const arm_state *state,
        uint32_t pc, unsigned *carry)
{
    if (opcode & 0x02000000)
    {
        unsigned rotation = ((opcode >> 8) & 15) * 2;
        uint32_t value = rotate_right(opcode & 255, rotation);
        if (rotation)
            *carry = value >> 31;
        return value;
    }

    bool by_register = (opcode & 0x10) != 0;
    unsigned kind = (opcode >> 5) & 3;
    unsigned rm = opcode & 15;
    uint32_t value = arm_reg(state, rm, pc, by_register ? 12 : 8);
    unsigned amount = by_register ?
            arm_reg(state, (opcode >> 8) & 15, pc, 8) & 255 :
            (opcode >> 7) & 31;

    if (by_register && amount == 0)
        return value;

    switch (kind)
    {
        case 0: /* LSL */
            if (amount == 0)
                return value;
            if (amount < 32)
            {
                *carry = (value >> (32 - amount)) & 1;
                return value << amount;
            }
            *carry = amount == 32 ? value & 1 : 0;
            return 0;
        case 1: /* LSR */
            if (amount == 0 || amount == 32)
            {
                *carry = value >> 31;
                return 0;
            }
            if (amount < 32)
            {
                *carry = (value >> (amount - 1)) & 1;
                return value >> amount;
            }
            *carry = 0;
            return 0;
        case 2: /* ASR */
            if (amount == 0 || amount >= 32)
            {
                *carry = value >> 31;
                return arithmetic_right(value, 32);
            }
            *carry = (value >> (amount - 1)) & 1;
            return arithmetic_right(value, amount);
        case 3: /* ROR / RRX */
            if (!by_register && amount == 0)
            {
                unsigned old_carry = *carry;
                *carry = value & 1;
                return (value >> 1) | (old_carry << 31);
            }
            amount &= 31;
            if (!amount)
            {
                *carry = value >> 31;
                return value;
            }
            *carry = (value >> (amount - 1)) & 1;
            return rotate_right(value, amount);
    }
    CHECK(0);
    return 0;
}

static void reference_arm_alu(uint32_t opcode, uint32_t pc, arm_state *state)
{
    unsigned condition = opcode >> 28;
    if (!arm_condition(condition, state->n, state->z, state->c, state->v))
        return;

    unsigned operation = (opcode >> 21) & 15;
    unsigned rn = (opcode >> 16) & 15;
    unsigned rd = (opcode >> 12) & 15;
    bool set_flags = (opcode & 0x00100000) != 0;
    unsigned old_carry = state->c;
    unsigned shifter_carry = old_carry;
    uint32_t a = arm_reg(state, rn, pc, 8);
    uint32_t b = reference_operand2(opcode, state, pc, &shifter_carry);
    uint32_t result = 0;
    bool arithmetic = false;
    bool subtraction = false;
    uint32_t left = a, right = b;
    unsigned carry = 0, overflow = 0;

    switch (operation)
    {
        case 0: case 8: result = a & b; break;
        case 1: case 9: result = a ^ b; break;
        case 2: case 10: subtraction = true; arithmetic = true; break;
        case 3: subtraction = true; arithmetic = true; left = b; right = a; break;
        case 4: case 11: arithmetic = true; break;
        case 5: arithmetic = true; carry = old_carry; break;
        case 6: subtraction = true; arithmetic = true;
            carry = 1 - old_carry; break;
        case 7: subtraction = true; arithmetic = true; left = b; right = a;
            carry = 1 - old_carry; break;
        case 12: result = a | b; break;
        case 13: result = b; break;
        case 14: result = a & ~b; break;
        case 15: result = ~b; break;
    }

    if (arithmetic)
    {
        int64_t signed_result;
        if (subtraction)
        {
            result = left - right - carry;
            signed_result = (int64_t)(int32_t)left -
                    (int64_t)(int32_t)right - carry;
            carry = (uint64_t)left >= (uint64_t)right + carry;
        }
        else
        {
            uint64_t wide = (uint64_t)left + right + carry;
            result = (uint32_t)wide;
            signed_result = (int64_t)(int32_t)left +
                    (int64_t)(int32_t)right + carry;
            carry = wide >> 32;
        }
        overflow = signed_result < INT32_MIN || signed_result > INT32_MAX;
    }

    if (set_flags)
    {
        state->n = result >> 31;
        state->z = result == 0;
        state->c = arithmetic ? carry : shifter_carry;
        if (arithmetic)
            state->v = overflow;
    }
    if (operation < 8 || operation > 11)
        state->reg[rd] = result;
}

static void check_against_reference(uint32_t opcode, uint32_t pc,
        const arm_state *initial)
{
    uint32_t storage[128];
    size_t size = emit_one(opcode, pc, storage);
    arm_state expected = *initial;
    reference_arm_alu(opcode, pc, &expected);

    uint32_t host[32] = {0};
    for (unsigned reg = 0; reg < 15; ++reg)
        host[mapped_reg[reg]] = initial->reg[reg];
    host[RV_FLAG_N] = initial->n;
    host[RV_FLAG_Z] = initial->z;
    host[RV_FLAG_C] = initial->c;
    host[RV_FLAG_V] = initial->v;
    host[RV_BUDGET] = 0x1234;
    host[RV_PC] = 0x5678;
    execute_emitted(storage, size, host);

    for (unsigned reg = 0; reg < 15; ++reg)
        if (host[mapped_reg[reg]] != expected.reg[reg])
        {
            fprintf(stderr, "opcode %08x pc %08x r%u: got %08x expected %08x\n",
                    opcode, pc, reg, host[mapped_reg[reg]], expected.reg[reg]);
            exit(1);
        }
    if (host[RV_FLAG_N] != expected.n || host[RV_FLAG_Z] != expected.z ||
        host[RV_FLAG_C] != expected.c || host[RV_FLAG_V] != expected.v)
    {
        fprintf(stderr, "opcode %08x pc %08x NZCV: got %u%u%u%u expected %u%u%u%u\n",
                opcode, pc, host[RV_FLAG_N], host[RV_FLAG_Z], host[RV_FLAG_C],
                host[RV_FLAG_V],
                expected.n, expected.z, expected.c, expected.v);
        exit(1);
    }
    CHECK(host[RV_BUDGET] == 0x1234 && host[RV_PC] == 0x5678);
}

static void test_executed_conditions(void)
{
    uint32_t storage[128];
    for (unsigned condition = 0; condition < 16; ++condition)
    {
        uint32_t opcode = (condition << 28) | 0x03a00001; /* MOV r0,#1 */
        size_t size = emit_one(opcode, 0x08000000, storage);
        for (unsigned flags = 0; flags < 16; ++flags)
        {
            uint32_t host[32] = {0};
            unsigned n = flags & 1;
            unsigned z = (flags >> 1) & 1;
            unsigned c = (flags >> 2) & 1;
            unsigned v = (flags >> 3) & 1;
            host[mapped_reg[0]] = 0x55;
            host[RV_FLAG_N] = n;
            host[RV_FLAG_Z] = z;
            host[RV_FLAG_C] = c;
            host[RV_FLAG_V] = v;
            host[RV_BUDGET] = 0x1234;
            host[RV_PC] = 0x5678;
            execute_emitted(storage, size, host);
            CHECK(host[mapped_reg[0]] ==
                    (arm_condition(condition, n, z, c, v) ? 1u : 0x55u));
            CHECK(host[RV_FLAG_N] == n && host[RV_FLAG_Z] == z &&
                    host[RV_FLAG_C] == c && host[RV_FLAG_V] == v);
            CHECK(host[RV_BUDGET] == 0x1234 && host[RV_PC] == 0x5678);
        }
    }
}

static void test_executed_arithmetic(void)
{
    static const uint32_t values[] = {
        0, 1, 0x7fffffff, 0x80000000, 0xffffffff,
    };
    static const struct { uint32_t opcode; unsigned operation; } cases[] = {
        {0xe0902001, 0}, /* ADDS r2,r0,r1 */
        {0xe0502001, 1}, /* SUBS r2,r0,r1 */
        {0xe1500001, 2}, /* CMP r0,r1 */
        {0xe0900001, 3}, /* ADDS r0,r0,r1: destination aliases a source */
    };
    uint32_t storage[128];
    for (size_t test = 0; test < sizeof(cases) / sizeof(cases[0]); ++test)
    {
        size_t size = emit_one(cases[test].opcode, 0x08000000, storage);
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
            for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); ++j)
            {
                uint32_t a = values[i], b = values[j];
                uint32_t host[32] = {0};
                host[mapped_reg[0]] = a;
                host[mapped_reg[1]] = b;
                host[mapped_reg[2]] = 0xa5a5a5a5;
                host[RV_BUDGET] = 0x1234;
                host[RV_PC] = 0x5678;
                execute_emitted(storage, size, host);

                bool addition = cases[test].operation == 0 ||
                        cases[test].operation == 3;
                uint32_t result = addition ? a + b : a - b;
                int64_t signed_result = addition ?
                        (int64_t)(int32_t)a + (int64_t)(int32_t)b :
                        (int64_t)(int32_t)a - (int64_t)(int32_t)b;
                unsigned carry = addition ? (uint64_t)a + b > UINT32_MAX : a >= b;
                unsigned overflow = signed_result < INT32_MIN ||
                        signed_result > INT32_MAX;
                unsigned destination = cases[test].operation == 3 ? 0 : 2;
                if (cases[test].operation != 2)
                    CHECK(host[mapped_reg[destination]] == result);
                else
                    CHECK(host[mapped_reg[2]] == 0xa5a5a5a5);
                CHECK(host[RV_FLAG_N] == result >> 31);
                CHECK(host[RV_FLAG_Z] == (result == 0));
                CHECK(host[RV_FLAG_C] == carry);
                CHECK(host[RV_FLAG_V] == overflow);
                CHECK(host[RV_BUDGET] == 0x1234 && host[RV_PC] == 0x5678);
            }
    }
}

static void test_executed_move_and_pc(void)
{
    uint32_t storage[128];
    static const struct { uint32_t opcode; uint32_t result; unsigned carry; }
            moves[] = {
        {0xe3b02080, 0x80, 1},       /* MOVS r2,#0x80: keep old C */
        {0xe3b02102, 0x80000000, 1}, /* MOVS r2,#0x80000000: rotated C */
        {0xe3b02101, 0x40000000, 0}, /* MOVS r2,#0x40000000: rotated C */
        {0xe1a0200f, 0x08000008, 1}, /* MOV r2,PC */
    };
    for (size_t i = 0; i < sizeof(moves) / sizeof(moves[0]); ++i)
    {
        size_t size = emit_one(moves[i].opcode, 0x08000000, storage);
        uint32_t host[32] = {0};
        host[RV_FLAG_C] = 1;
        host[RV_FLAG_V] = 1;
        execute_emitted(storage, size, host);
        CHECK(host[mapped_reg[2]] == moves[i].result);
        CHECK(host[RV_FLAG_C] == moves[i].carry && host[RV_FLAG_V] == 1);
    }

    size_t size = emit_one(0xe08f2001, 0x08000000, storage); /* ADD r2,PC,r1 */
    uint32_t host[32] = {0};
    host[mapped_reg[1]] = 7;
    execute_emitted(storage, size, host);
    CHECK(host[mapped_reg[2]] == 0x0800000f);
}

static void test_pc_destination(void)
{
    uint32_t storage[128];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, storage, sizeof(storage));
    CHECK(gpsp_rv32_emit_arm_alu(&code, 0xe1a0f00e, 0x08000000) ==
            GPSP_RV32_ARM_ALU_EXIT); /* MOV pc,lr */
    uint32_t host[32] = {0};
    host[mapped_reg[14]] = 0x08001237;
    execute_emitted(storage, code.ptr - code.start, host);
    CHECK(host[10] == 0x08001234 && host[11] == 1);

    gpsp_rv32_code_init(&code, storage, sizeof(storage));
    CHECK(gpsp_rv32_emit_arm_alu(&code, 0x0080f001, 0x08000000) ==
            GPSP_RV32_ARM_ALU_EXIT); /* ADDEQ pc,r0,r1 */
    memset(host, 0, sizeof(host));
    host[mapped_reg[0]] = 0x08001000;
    host[mapped_reg[1]] = 0x23;
    execute_emitted(storage, code.ptr - code.start, host);
    CHECK(host[10] == 0x08000004 && host[11] == 0);
    memset(host, 0, sizeof(host));
    host[RV_FLAG_Z] = 1; /* Z */
    host[mapped_reg[0]] = 0x08001000;
    host[mapped_reg[1]] = 0x23;
    execute_emitted(storage, code.ptr - code.start, host);
    CHECK(host[10] == 0x08001020 && host[11] == 1);
}

static uint32_t arm_base_opcode(unsigned operation, bool set_flags,
        unsigned rn, unsigned rd)
{
    return 0xe0000000u | (operation << 21) | (set_flags ? 0x00100000u : 0) |
            (rn << 16) | (rd << 12);
}

static arm_state sample_state(unsigned sample, unsigned shift_amount)
{
    static const uint32_t values[] = {
        0, 1, 0x7fffffff, 0x80000000, 0xffffffff,
        0x12345678, 0x87654321, 0x80000001,
    };
    arm_state state = {0};
    for (unsigned i = 0; i < 15; ++i)
        state.reg[i] = 0x11111111u * (i + 1);
    state.reg[0] = values[sample];
    state.reg[1] = values[(sample + 3) & 7];
    state.reg[2] = 0xa5a5a5a5;
    state.reg[3] = shift_amount;
    state.n = sample & 1;
    state.z = (sample >> 1) & 1;
    state.c = (sample >> 2) & 1;
    state.v = (sample >> 3) & 1;
    return state;
}

static void test_all_operations_and_shifts(void)
{
    static const unsigned shift_amounts[] = {
        0, 1, 31, 32, 33, 64, 255, 256, 257,
    };
    static const struct { unsigned rotate, imm8; } immediates[] = {
        {0, 0}, {0, 255}, {1, 2}, {8, 0x81}, {15, 0x81},
    };
    unsigned cases = 0;
    for (unsigned operation = 0; operation < 16; ++operation)
        for (unsigned set_flags = 0; set_flags < 2; ++set_flags)
        {
            if (operation >= 8 && operation <= 11 && !set_flags)
                continue;
            unsigned rd = operation >= 8 && operation <= 11 ? 0 : 2;
            uint32_t base = arm_base_opcode(operation, set_flags != 0, 0, rd);
            for (unsigned sample = 0; sample < 8; ++sample)
            {
                arm_state state = sample_state(sample, 0);
                for (size_t i = 0; i < sizeof(immediates) / sizeof(immediates[0]); ++i)
                {
                    uint32_t opcode = base | 0x02000000u |
                            (immediates[i].rotate << 8) | immediates[i].imm8;
                    check_against_reference(opcode, 0x08000000, &state);
                    cases++;
                }
                for (unsigned kind = 0; kind < 4; ++kind)
                {
                    static const unsigned immediate_counts[] = {0, 1, 31};
                    for (size_t i = 0; i < sizeof(immediate_counts) /
                            sizeof(immediate_counts[0]); ++i)
                    {
                        uint32_t opcode = base | (immediate_counts[i] << 7) |
                                (kind << 5) | 1;
                        check_against_reference(opcode, 0x08000000, &state);
                        cases++;
                    }
                    for (size_t i = 0; i < sizeof(shift_amounts) /
                            sizeof(shift_amounts[0]); ++i)
                    {
                        state.reg[3] = shift_amounts[i];
                        uint32_t opcode = base | (3u << 8) | (kind << 5) | 0x10 | 1;
                        check_against_reference(opcode, 0x08000000, &state);
                        cases++;
                    }
                }
            }
        }
    printf("%u ARM opcode/shift state comparisons passed\n", cases);
}

static void test_pc_and_alias_sources(void)
{
    arm_state state = sample_state(5, 1);
    uint32_t pc = 0x08000000;
    check_against_reference(arm_base_opcode(13, true, 0, 2) | 15,
            pc, &state); /* MOVS r2,PC: +8 */
    check_against_reference(arm_base_opcode(13, true, 0, 2) |
            (3u << 8) | 0x10 | 15, pc, &state); /* Rm PC, register shift: +12 */
    check_against_reference(arm_base_opcode(4, true, 15, 2) |
            (3u << 8) | 0x10 | 1, pc, &state); /* Rn PC: +8 */

    for (unsigned operation = 0; operation < 16; ++operation)
    {
        unsigned rd = operation >= 8 && operation <= 11 ? 0 : 1;
        uint32_t opcode = arm_base_opcode(operation, true, 0, rd) |
                (2u << 8) | (3u << 5) | 0x10 | 1;
        check_against_reference(opcode, pc, &state);
    }

    /* A failed condition must suppress destination, shifter C and NZV. */
    uint32_t eq_adds = arm_base_opcode(4, true, 0, 2) | 0x02000001;
    eq_adds &= 0x0fffffff;
    state.z = 0;
    check_against_reference(eq_adds, pc, &state);
    state.z = 1;
    check_against_reference(eq_adds, pc, &state);
    uint32_t eq_adds_alias = arm_base_opcode(4, true, 0, 0) |
            0x02000001u;
    eq_adds_alias &= 0x0fffffffu;
    state.z = 0;
    check_against_reference(eq_adds_alias, pc, &state);
    state.z = 1;
    check_against_reference(eq_adds_alias, pc, &state);

    /* Direct destination writes must also work when Rd aliases Rn or Rm. */
    state = sample_state(6, 1);
    check_against_reference(arm_base_opcode(4, false, 0, 0) | 1,
            pc, &state); /* ADD r0,r0,r1 */
    check_against_reference(arm_base_opcode(2, false, 0, 1) | 1,
            pc, &state); /* SUB r1,r0,r1 */
    check_against_reference(arm_base_opcode(14, false, 0, 1) | 1,
            pc, &state); /* BIC r1,r0,r1 */
    check_against_reference(arm_base_opcode(4, false, 0, 0) |
            0x02000001u, pc, &state); /* ADD r0,r0,#1 */
}

static size_t emit_one(uint32_t opcode, uint32_t pc, uint32_t *storage)
{
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, storage, 128 * sizeof(uint32_t));
    CHECK(gpsp_rv32_emit_arm_alu(&code, opcode, pc) ==
            GPSP_RV32_ARM_ALU_EMITTED);
    size_t size = (size_t)(code.ptr - code.start);

    /* Scheduling and host ABI state are owned by the block caller/stub. */
    for (size_t i = 0, length; i < size; i += length)
    {
        uint32_t word = 0;
        length = rv32_rvc_fetch(storage, size, i, &word);
        CHECK(length);
        if ((word & 0x7f) == 0x63)
            continue;
        unsigned rd = (word >> 7) & 31;
        CHECK(rd != RA && rd != 2 && rd != 3 && rd != 4 && rd != RV_STATE &&
                rd != RV_BUDGET && rd != RV_PC);
    }
    return size;
}

#if GPSP_RV32_USE_ZBB
static void test_zbb_rotate_encoding(void)
{
    uint32_t storage[128];
    uint32_t opcodes[] = {0xe1a000e1, 0xe1a00371}; /* ROR #1, ROR r3 */
    uint32_t rv_opcodes[] = {0x13, 0x33}; /* RORI, ROR */
    for (unsigned kind = 0; kind < 2; ++kind)
    {
        size_t size = emit_one(opcodes[kind], 0x08000000, storage);
        bool found = false;
        for (size_t i = 0; i < size / 4; ++i)
            found |= (storage[i] & 0xfe00707f) ==
                    ((0x30u << 25) | (5u << 12) | rv_opcodes[kind]);
        CHECK(found);
    }
}
#endif

static void test_supported_forms(void)
{
    uint32_t storage[128];
    CHECK(emit_one(0xe3a00001, 0x08000000, storage) == 4); /* MOV r0,#1 */
    CHECK(emit_one(0xe3b00102, 0x08000000, storage) == 20); /* MOVS rotate */
    CHECK(emit_one(0xe0802001, 0x08000000, storage) == 4); /* ADD r2,r0,r1 */
    uint32_t add = read_word((uint8_t *)storage);
    CHECK((add & 0x7f) == 0x33 && (add >> 7 & 31) == mapped_reg[2] &&
            (add >> 15 & 31) == mapped_reg[0] &&
            (add >> 20 & 31) == mapped_reg[1]);
    CHECK(emit_one(0xe2800001, 0x08000000, storage) == 4); /* ADD r0,r0,#1 */
    uint32_t addi = read_word((uint8_t *)storage);
    CHECK((addi & 0x7f) == 0x13 && (addi >> 7 & 31) == mapped_reg[0] &&
            (addi >> 15 & 31) == mapped_reg[0] && (addi >> 20) == 1);
    CHECK(emit_one(0xe1a00081, 0x08000000, storage) == 8); /* MOV LSL #1 */
    CHECK(emit_one(0xe1b00081, 0x08000000, storage) == 20); /* MOVS LSL #1 */
    CHECK(emit_one(0xe1a00000, 0x08000000, storage) == 0); /* MOV r0,r0 */
    CHECK(emit_one(0xe2800000, 0x08000000, storage) == 0); /* ADD #0 */
    CHECK(emit_one(0xe2400000, 0x08000000, storage) == 0); /* SUB #0 */
    CHECK(emit_one(0xe3c00000, 0x08000000, storage) == 0); /* BIC #0 */
    CHECK(emit_one(0x01a00000, 0x08000000, storage) == 0); /* MOVEQ r0,r0 */
#if GPSP_RV32_USE_ZBB
    CHECK(emit_one(0xe1c01001, 0x08000000, storage) == 4); /* BIC r1,r0,r1 */
    CHECK((read_word((uint8_t *)storage) >> 25) == 0x20);
#endif
#if GPSP_RV32_USE_ZBA
    CHECK(emit_one(0xe0802101, 0x08000000, storage) == 4); /* ADD LSL #2 */
    CHECK((storage[0] & 0xfe00707f) == ((0x10u << 25) | (4u << 12) | 0x33));
#endif
#if GPSP_RV32_USE_ZBS
    {
        size_t size = emit_one(0xe1b00101, 0x08000000, storage); /* MOVS LSL #2 */
        bool bexti = false;
        for (size_t i = 0; i < size / 4; ++i)
            bexti |= (storage[i] & 0xfe00707f) == ((0x24u << 25) | (5u << 12) | 0x13);
        CHECK(bexti);
    }
#endif
#if GPSP_RV32_USE_ZBB
    CHECK(emit_one(0xe0902001, 0x08000000, storage) == 32); /* ADDS */
    bool xnor = false;
    for (unsigned i = 0; i < 8; ++i)
        xnor |= (storage[i] & 0xfe00707f) == ((0x20u << 25) | (4u << 12) | 0x33);
    CHECK(xnor);
#else
    CHECK(emit_one(0xe0902001, 0x08000000, storage) == 36); /* ADDS */
#endif
    CHECK(emit_one(0xe0502001, 0x08000000, storage) == 36); /* SUBS */
    CHECK(emit_one(0xe3500001, 0x08000000, storage) == 40); /* CMP #1 */
    CHECK(emit_one(0xe28f0004, 0x08000000, storage) == 16); /* ADD PC,#4 */
    CHECK(emit_one(0xe1a0000f, 0x08000000, storage) == 12); /* MOV r0,PC */

    for (unsigned condition = 0; condition < 16; ++condition)
    {
        uint32_t opcode = (condition << 28) | 0x03a00001; /* MOV r0,#1 */
        size_t size = emit_one(opcode, 0x08000000, storage);
        if (condition == 14)
        {
            CHECK(size == 4);
            continue;
        }
        if (condition == 15)
        {
            CHECK(size == 0);
            continue;
        }

        unsigned branches = 0;
        for (size_t i = 0; i < size; i += 4)
        {
            uint32_t word = read_word((uint8_t *)storage + i);
            if ((word & 0x7f) == 0x63)
            {
                CHECK((word >> 12 & 7) == 0 || (word >> 12 & 7) == 1);
                CHECK((int32_t)i + branch_offset(word) >= 0);
                CHECK((int32_t)i + branch_offset(word) <= (int32_t)size);
                branches++;
            }
        }
        CHECK(branches == ((condition == 8 || condition == 9 ||
                condition == 12 || condition == 13) ? 2u : 1u));
    }
}

static void test_block_pc_reads(void)
{
    static const struct { uint32_t opcode, expected; } cases[] = {
        {0xe1a0000f, 0x08000008}, /* MOV r0,PC */
        {0xe28f0004, 0x0800000c}, /* ADD r0,PC,#4 */
        {0xe1a0031f, 0x10000018}, /* MOV r0,PC,LSL r3 */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        uint32_t storage[128];
        gpsp_rv32_code code;
        gpsp_rv32_code_init(&code, storage, sizeof(storage));
        CHECK(gpsp_rv32_emit_arm_alu_block(&code, cases[i].opcode,
                0x08000000) == GPSP_RV32_ARM_ALU_EMITTED);
        size_t size = (size_t)(code.ptr - code.start);
        uint32_t host[32] = {0};
        host[RV_PC] = 0x08000000;
        host[mapped_reg[3]] = 1;
        execute_emitted(storage, size, host);
        CHECK(host[mapped_reg[0]] == cases[i].expected);
        CHECK(size + 4 == emit_one(cases[i].opcode, 0x08000000, storage));
    }
}

static void check_unsupported(uint32_t opcode, uint32_t pc)
{
    uint32_t storage[128];
    memset(storage, 0xa5, sizeof(storage));
    uint8_t before[sizeof(storage)];
    memcpy(before, storage, sizeof(storage));
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, storage, sizeof(storage));
    uint8_t *cursor = code.ptr;
    CHECK(gpsp_rv32_emit_arm_alu(&code, opcode, pc) ==
            GPSP_RV32_ARM_ALU_UNSUPPORTED);
    CHECK(code.ptr == cursor);
    CHECK(memcmp(storage, before, sizeof(storage)) == 0);
}

static void test_failure_atomicity(void)
{
    CHECK(gpsp_rv32_emit_arm_alu(NULL, 0xf3a00001, 0x08000000) ==
            GPSP_RV32_ARM_ALU_BUFFER_FULL); /* cond=15 still needs a valid output */
    check_unsupported(0xe1a0009f, 0x08000000); /* bit 7 overlap */
    check_unsupported(0xe1b0f000, 0x08000000); /* SPSR restore */
    check_unsupported(0xe0000091, 0x08000000); /* MUL overlap */
    check_unsupported(0xe5910000, 0x08000000); /* LDR */
    check_unsupported(0xe1400001, 0x08000000); /* CMP without S */
    check_unsupported(0xe3501001, 0x08000000); /* CMP reserved Rd */
    check_unsupported(0xe3a10001, 0x08000000); /* MOV reserved Rn */
    check_unsupported(arm_base_opcode(13, true, 0, 2) |
            (15u << 8) | 0x10 | 1, 0x08000000); /* Rs PC is undefined */
    check_unsupported(0xe10f0000, 0x08000000); /* MRS CPSR overlap */
    check_unsupported(0xe14f0000, 0x08000000); /* MRS SPSR overlap */
    check_unsupported(0xe12fff1e, 0x08000000); /* BX overlap */
    check_unsupported(0xe3a00001, 0x08000002); /* unaligned ARM PC */

    uint32_t storage[128];
    memset(storage, 0xa5, sizeof(storage));
    uint8_t before[sizeof(storage)];
    memcpy(before, storage, sizeof(storage));
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, storage, 0);
    uint8_t *cursor = code.ptr;
    CHECK(gpsp_rv32_emit_arm_alu(&code, 0xe3a00001, 0x08000000) ==
            GPSP_RV32_ARM_ALU_BUFFER_FULL);
    CHECK(code.ptr == cursor);
    CHECK(memcmp(storage, before, sizeof(storage)) == 0);

    gpsp_rv32_code_init(&code, storage, 16);
    cursor = code.ptr;
    uint32_t shifted_adcs = arm_base_opcode(5, true, 0, 2) |
            (3u << 8) | (3u << 5) | 0x10 | 1;
    CHECK(gpsp_rv32_emit_arm_alu(&code, shifted_adcs, 0x08000000) ==
            GPSP_RV32_ARM_ALU_BUFFER_FULL);
    CHECK(code.ptr == cursor);
    CHECK(memcmp(storage, before, sizeof(storage)) == 0);

    /* The emitter stages in a child; the parent policy stays as it was on
       success and on failure. */
    gpsp_rv32_code_init(&code, storage, 16);
    code.compress = true;
    code.fixed_depth = 2;
    CHECK(gpsp_rv32_emit_arm_alu(&code, shifted_adcs, 0x08000000) ==
            GPSP_RV32_ARM_ALU_BUFFER_FULL);
    CHECK(code.compress && code.fixed_depth == 2);
    gpsp_rv32_code_init(&code, storage, sizeof(storage));
    code.compress = true;
    code.fixed_depth = 2;
    CHECK(gpsp_rv32_emit_arm_alu(&code, shifted_adcs, 0x08000000) ==
            GPSP_RV32_ARM_ALU_EMITTED);
    CHECK(code.compress && code.fixed_depth == 2);
}

int main(void)
{
    /* Exact sizes below assume 32-bit instructions. */
    gpsp_rv32_test_compress = false;
    test_supported_forms();
    test_block_pc_reads();
    test_executed_conditions();
    test_executed_arithmetic();
    test_executed_move_and_pc();
    test_pc_destination();
    test_all_operations_and_shifts();
#if GPSP_RV32_USE_ZBB
    test_zbb_rotate_encoding();
#endif
    test_pc_and_alias_sources();
    test_failure_atomicity();

    /* The same models with 16-bit forms in the emitted code. */
    gpsp_rv32_test_compress = true;
    uint32_t storage[128];
    /* c.sub and c.and need r0 and r1 in x8-x15. */
    size_t pair = guest_reg_map[0] >= 8 && guest_reg_map[0] <= 15 &&
            guest_reg_map[1] >= 8 && guest_reg_map[1] <= 15 ? 2 : 4;
    CHECK(emit_one(0xe3a00001, 0x08000000, storage) == 2);  /* MOV r0,#1 */
    CHECK(emit_one(0xe0400001, 0x08000000, storage) == pair); /* SUB r0,r0,r1 */
    CHECK(emit_one(0xe0000001, 0x08000000, storage) == pair); /* AND r0,r0,r1 */
    test_executed_conditions();
    test_executed_arithmetic();
    test_executed_move_and_pc();
    test_all_operations_and_shifts();
    gpsp_rv32_test_compress = false;
    puts("ARM data-processing execution model, conditions and rollback passed");
    return 0;
}
