/* Shared inputs for the real gpSP interpreter and the RV32 execution test.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_RV32_ARM_MUL_CASES_H
#define GPSP_RV32_ARM_MUL_CASES_H

#include <stdbool.h>
#include <stdint.h>

#define MUL_PC UINT32_C(0x06000000)
#define ARM_MUL_SHORT(cond, acc, s, rd, rn, rs, rm) \
    (((uint32_t)(cond) << 28) | ((uint32_t)(acc) << 21) | \
            ((uint32_t)(s) << 20) | ((uint32_t)(rd) << 16) | \
            ((uint32_t)(rn) << 12) | ((uint32_t)(rs) << 8) | 0x90u | (rm))
#define ARM_MUL_LONG(cond, sign, acc, s, hi, lo, rs, rm) \
    (((uint32_t)(cond) << 28) | 0x00800090u | \
            ((uint32_t)(sign) << 22) | ((uint32_t)(acc) << 21) | \
            ((uint32_t)(s) << 20) | ((uint32_t)(hi) << 16) | \
            ((uint32_t)(lo) << 12) | ((uint32_t)(rs) << 8) | (rm))

typedef struct
{
    unsigned reg;
    uint32_t value;
} mul_override;

typedef struct
{
    const char *name;
    uint32_t opcode;
    uint32_t flags;
    unsigned override_count;
    mul_override overrides[5];
} mul_case;

static const mul_case mul_cases[] = {
    {"MUL", ARM_MUL_SHORT(14, 0, 0, 4, 0, 1, 0), 0xA0000000, 3,
        {{0, 3}, {1, 7}, {4, 0xDEADBEEF}}},
    {"MULS negative", ARM_MUL_SHORT(14, 0, 1, 4, 0, 1, 0), 0x30000000, 2,
        {{0, 0xFFFFFFFF}, {1, 2}}},
    {"MULS zero", ARM_MUL_SHORT(14, 0, 1, 4, 0, 1, 0), 0x20000000, 2,
        {{0, 0}, {1, 0xFFFFFFFF}}},
    {"MLA wrap", ARM_MUL_SHORT(14, 1, 0, 4, 2, 1, 0), 0xC0000000, 3,
        {{0, 0xFFFFFFFF}, {1, 2}, {2, 5}}},
    {"MLAS Rd=Rm", ARM_MUL_SHORT(14, 1, 1, 4, 2, 1, 4), 0x30000000, 3,
        {{4, 0x7FFFFFFF}, {1, 2}, {2, 5}}},
    {"MUL Rd=Rs", ARM_MUL_SHORT(14, 0, 0, 1, 0, 1, 0), 0xA0000000, 2,
        {{0, 0xFFFFFFFF}, {1, 3}}},
    {"MUL all sources=Rd", ARM_MUL_SHORT(14, 0, 0, 4, 0, 4, 4), 0xA0000000, 1,
        {{4, 0xFFFFFFFF}}},
    {"MLA Rd=Rn", ARM_MUL_SHORT(14, 1, 0, 2, 2, 1, 0), 0x50000000, 3,
        {{0, 7}, {1, 8}, {2, 9}}},
    {"MLAS all sources=Rd", ARM_MUL_SHORT(14, 1, 1, 4, 4, 4, 4), 0x20000000, 1,
        {{4, 3}}},
    {"UMULL max", ARM_MUL_LONG(14, 0, 0, 0, 5, 4, 1, 0), 0x30000000, 2,
        {{0, 0xFFFFFFFF}, {1, 0xFFFFFFFF}}},
    {"UMULL Lo=Rm", ARM_MUL_LONG(14, 0, 0, 0, 5, 4, 1, 4), 0x30000000, 2,
        {{4, 0xFFFFFFFF}, {1, 3}}},
    {"SMULL Lo=Rs", ARM_MUL_LONG(14, 1, 0, 0, 5, 4, 4, 0), 0x30000000, 2,
        {{0, 0x80000000}, {4, 3}}},
    {"UMULL Hi=Rm", ARM_MUL_LONG(14, 0, 0, 0, 4, 5, 1, 4), 0x30000000, 2,
        {{4, 0xFFFFFFFF}, {1, 3}}},
    {"UMULLS zero", ARM_MUL_LONG(14, 0, 0, 1, 5, 4, 1, 0), 0xA0000000, 2,
        {{0, 0}, {1, 0xFFFFFFFF}}},
    {"UMLAL carry", ARM_MUL_LONG(14, 0, 1, 0, 5, 4, 1, 0), 0xA0000000, 4,
        {{0, 1}, {1, 1}, {4, 0xFFFFFFFF}, {5, 0x1234}}},
    {"UMLALS aliases", ARM_MUL_LONG(14, 0, 1, 1, 5, 4, 5, 4), 0x10000000, 2,
        {{4, 0xFFFFFFFE}, {5, 2}}},
    {"SMULL negative", ARM_MUL_LONG(14, 1, 0, 0, 5, 4, 1, 0), 0x50000000, 2,
        {{0, 0xFFFFFFFF}, {1, 2}}},
    {"SMULLS zero", ARM_MUL_LONG(14, 1, 0, 1, 5, 4, 1, 0), 0xA0000000, 2,
        {{0, 0x80000000}, {1, 0}}},
    {"SMLALS aliases", ARM_MUL_LONG(14, 1, 1, 1, 5, 4, 4, 5), 0x10000000, 2,
        {{4, 2}, {5, 0xFFFFFFFF}}},
    {"SMLAL zero", ARM_MUL_LONG(14, 1, 1, 1, 5, 4, 1, 0), 0x30000000, 4,
        {{0, 0xFFFFFFFF}, {1, 1}, {4, 1}, {5, 0}}},
    {"SMLALS min", ARM_MUL_LONG(14, 1, 1, 1, 5, 4, 1, 0), 0x30000000, 4,
        {{0, 0x80000000}, {1, 2}, {4, 0xFFFFFFFF}, {5, 0}}},
    {"SP/LR result", ARM_MUL_LONG(14, 0, 0, 1, 14, 13, 9, 8), 0x10000000, 4,
        {{8, 0x80000000}, {9, 3}, {13, 0x3333}, {14, 0x4444}}},
    {"EQ pass", ARM_MUL_SHORT(0, 0, 1, 4, 0, 1, 0), 0x60000000, 2,
        {{0, 3}, {1, 5}}},
    {"EQ fail", ARM_MUL_SHORT(0, 0, 1, 4, 0, 1, 0), 0x20000000, 2,
        {{0, 3}, {1, 5}}},
    {"GT pass", ARM_MUL_LONG(12, 1, 0, 1, 5, 4, 1, 0), 0x90000000, 2,
        {{0, 0xFFFFFFFF}, {1, 2}}},
    {"GT fail", ARM_MUL_LONG(12, 1, 0, 1, 5, 4, 1, 0), 0xD0000000, 2,
        {{0, 0xFFFFFFFF}, {1, 2}}},
    {"NV", ARM_MUL_SHORT(15, 0, 1, 4, 0, 1, 0), 0xF0000000, 2,
        {{0, 3}, {1, 5}}},
};

#define MUL_CASE_COUNT (sizeof(mul_cases) / sizeof(mul_cases[0]))

static void mul_case_init_regs(const mul_case *test, uint32_t regs[64])
{
    for (unsigned i = 0; i < 64; ++i)
        regs[i] = 0;
    for (unsigned i = 0; i < 15; ++i)
        regs[i] = 0x100u + i;
    for (unsigned i = 0; i < test->override_count; ++i)
        regs[test->overrides[i].reg] = test->overrides[i].value;
    regs[15] = MUL_PC;
    regs[16] = test->flags | 0x1Fu;
    regs[17] = 0x10; /* gpSP MODE_SYSTEM enum, not CPSR mode bits. */
}

/* 64-bit arithmetic is the independent expected-value path for the RV32M
 * two-half implementation. The host test also checks it against gpSP's real
 * one-instruction interpreter. */
static void mul_case_reference(const mul_case *test, uint32_t regs[64])
{
    uint32_t op = test->opcode;
    unsigned cond = op >> 28;
    unsigned n = regs[16] >> 31;
    unsigned z = (regs[16] >> 30) & 1;
    unsigned c = (regs[16] >> 29) & 1;
    unsigned v = (regs[16] >> 28) & 1;
    bool execute = cond == 14;
    switch (cond)
    {
        case 0: execute = z != 0; break;
        case 1: execute = z == 0; break;
        case 2: execute = c != 0; break;
        case 3: execute = c == 0; break;
        case 4: execute = n != 0; break;
        case 5: execute = n == 0; break;
        case 6: execute = v != 0; break;
        case 7: execute = v == 0; break;
        case 8: execute = c && !z; break;
        case 9: execute = !c || z; break;
        case 10: execute = n == v; break;
        case 11: execute = n != v; break;
        case 12: execute = !z && n == v; break;
        case 13: execute = z || n != v; break;
        case 15: execute = false; break;
    }

    if (execute)
    {
        unsigned rm = op & 15;
        unsigned rs = (op >> 8) & 15;
        bool accumulate = (op & 0x00200000u) != 0;
        bool set_flags = (op & 0x00100000u) != 0;
        if (op & 0x00800000u)
        {
            unsigned hi = (op >> 16) & 15;
            unsigned lo = (op >> 12) & 15;
            uint64_t product = (op & 0x00400000u) ?
                    (uint64_t)((int64_t)(int32_t)regs[rm] * (int64_t)(int32_t)regs[rs]) :
                    (uint64_t)regs[rm] * (uint64_t)regs[rs];
            if (accumulate)
                product += ((uint64_t)regs[hi] << 32) | regs[lo];
            regs[lo] = (uint32_t)product;
            regs[hi] = (uint32_t)(product >> 32);
            if (set_flags)
                regs[16] = (regs[16] & 0x3FFFFFFFu) |
                        (regs[hi] & 0x80000000u) |
                        (product == 0 ? 0x40000000u : 0);
        }
        else
        {
            unsigned rd = (op >> 16) & 15;
            unsigned rn = (op >> 12) & 15;
            uint32_t result = (uint32_t)((uint64_t)regs[rm] * regs[rs]);
            if (accumulate)
                result += regs[rn];
            regs[rd] = result;
            if (set_flags)
                regs[16] = (regs[16] & 0x3FFFFFFFu) |
                        (result & 0x80000000u) |
                        (result == 0 ? 0x40000000u : 0);
        }
    }
    regs[15] += 4;
}

#endif
