/* ARM B/BL/BX emission tests with a small RV32I executor.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_arm_branch.h"
#include "rv32_register_map.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../rv32-rvc.h"

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

enum { R0 = GPSP_RV32_REG_GUEST_R0, N = RV_FLAG_N, Z = RV_FLAG_Z,
        C = RV_FLAG_C, V = RV_FLAG_V, LR = RV_GUEST_LR };

static int32_t sign_extend(uint32_t value, unsigned bits)
{
    uint32_t sign = 1u << (bits - 1);
    return (int32_t)((value ^ sign) - sign);
}

static int32_t branch_offset(uint32_t word)
{
    uint32_t value = ((word >> 31) << 12) | (((word >> 7) & 1) << 11) |
            (((word >> 25) & 0x3f) << 5) | (((word >> 8) & 15) << 1);
    return sign_extend(value, 13);
}

static void execute_rv32(const uint32_t *words, size_t size, uint32_t host[32])
{
    size_t pc = 0;
    unsigned steps = 0;
    while (pc < size)
    {
        CHECK(++steps < 100 && !(pc & 1));
        uint32_t word = 0;
        unsigned length = rv32_rvc_fetch(words, size, pc, &word);
        CHECK(length);
        if (!length)
            return;
        unsigned opcode = word & 0x7f;
        unsigned rd = (word >> 7) & 31;
        unsigned funct3 = (word >> 12) & 7;
        unsigned rs1 = (word >> 15) & 31;
        unsigned rs2 = (word >> 20) & 31;
        unsigned funct7 = word >> 25;
        int32_t imm = sign_extend(word >> 20, 12);
        size_t next = pc + length;

        switch (opcode)
        {
            case 0x13:
                switch (funct3)
                {
                    case 0: host[rd] = host[rs1] + (uint32_t)imm; break;
                    case 4: host[rd] = host[rs1] ^ (uint32_t)imm; break;
                    case 7: host[rd] = host[rs1] & (uint32_t)imm; break;
                    default: CHECK(0);
                }
                break;
            case 0x33:
                CHECK(funct7 == 0);
                switch (funct3)
                {
                    case 4: host[rd] = host[rs1] ^ host[rs2]; break;
                    case 6: host[rd] = host[rs1] | host[rs2]; break;
                    case 7: host[rd] = host[rs1] & host[rs2]; break;
                    default: CHECK(0);
                }
                break;
            case 0x37: host[rd] = word & 0xfffff000; break;
            case 0x63:
            {
                CHECK(funct3 <= 1);
                if ((host[rs1] == host[rs2]) == (funct3 == 0))
                {
                    int64_t target = (int64_t)pc + branch_offset(word);
                    CHECK(target >= 0 && target <= (int64_t)size);
                    next = (size_t)target;
                }
                break;
            }
            default: CHECK(0);
        }
        host[0] = 0;
        pc = next;
    }
    CHECK(pc == size);
}

static void run(uint32_t opcode, uint32_t guest_pc, uint32_t host[32])
{
    uint32_t words[64];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(gpsp_rv32_emit_arm_branch(&code, opcode, guest_pc) ==
            GPSP_RV32_ARM_BRANCH_EXIT);
    execute_rv32(words, (size_t)(code.ptr - code.start), host);
}

static bool condition(unsigned code, unsigned n, unsigned z,
        unsigned c, unsigned v)
{
    switch (code)
    {
        case 0: return z; case 1: return !z;
        case 2: return c; case 3: return !c;
        case 4: return n; case 5: return !n;
        case 6: return v; case 7: return !v;
        case 8: return c && !z; case 9: return !c || z;
        case 10: return n == v; case 11: return n != v;
        case 12: return !z && n == v; case 13: return z || n != v;
        case 14: return true; default: return false;
    }
}

static void check_exit_adapter(uint32_t host[32], uint32_t *cpsr)
{
    host[RV_PC] = host[A0];
    *cpsr = (*cpsr & ~0x20u) | (host[A1] ? 0x20u : 0);
}

static void test_conditions_and_link()
{
    const uint32_t pc = 0x08000100;
    for (unsigned cond = 0; cond < 16; ++cond)
        for (unsigned flags = 0; flags < 16; ++flags)
            for (unsigned link = 0; link < 2; ++link)
            {
                uint32_t host[32] = {0};
                host[N] = flags & 1;
                host[Z] = (flags >> 1) & 1;
                host[C] = (flags >> 2) & 1;
                host[V] = (flags >> 3) & 1;
                host[LR] = 0xfeedbeef;
                uint32_t opcode = (cond << 28) | 0x0a000002 | (link << 24);
                run(opcode, pc, host);
                bool taken = condition(cond, host[N], host[Z], host[C], host[V]);
                CHECK(host[A0] == (taken ? pc + 16 : pc + 4));
                CHECK(host[A1] == 0);
                CHECK(host[LR] == (link && taken ? pc + 4 : 0xfeedbeef));
                CHECK(host[N] == (flags & 1));
                CHECK(host[Z] == ((flags >> 1) & 1));
                CHECK(host[C] == ((flags >> 2) & 1));
                CHECK(host[V] == ((flags >> 3) & 1));
                uint32_t cpsr = 0x30;
                check_exit_adapter(host, &cpsr);
                CHECK(host[RV_PC] == host[A0] && (cpsr & 0x20) == 0);
            }
}

static void test_displacements()
{
    const uint32_t pc = 0x08000100;
    uint32_t host[32] = {0};
    run(0xea7fffff, pc, host);
    CHECK(host[A0] == pc + 8 + 0x1fffffcu);
    run(0xea800000, pc, host);
    CHECK(host[A0] == pc + 8 - 0x2000000u);
    run(0xeaffffff, pc, host);
    CHECK(host[A0] == pc + 4);
}

static void test_bx()
{
    const uint32_t pc = 0x08000100;
    uint32_t host[32] = {0};
    host[R0] = 0x08000203;
    host[LR] = 0xfeedbeef;
    run(0xe12fff10, pc, host); /* BX r0 */
    CHECK(host[A0] == 0x08000202 && host[A1] == 1);
    CHECK(host[LR] == 0xfeedbeef);
    uint32_t cpsr = 0x10;
    check_exit_adapter(host, &cpsr);
    CHECK(host[RV_PC] == 0x08000202 && (cpsr & 0x20));

    host[R0] = 0x08000202;
    run(0xe12fff10, pc, host);
    CHECK(host[A0] == 0x08000202 && host[A1] == 0);
    cpsr = 0x30;
    check_exit_adapter(host, &cpsr);
    CHECK((cpsr & 0x20) == 0);

    host[Z] = 0;
    host[LR] = 0x08000401;
    run(0x012fff1e, pc, host); /* BXEQ LR: false */
    CHECK(host[A0] == pc + 4 && host[A1] == 0);
    CHECK(host[LR] == 0x08000401);
    host[Z] = 1;
    run(0x012fff1e, pc, host);
    CHECK(host[A0] == 0x08000400 && host[A1] == 1);
}

static void test_helper_and_buffer_rollback()
{
    static const uint32_t helper[] = {
        0xe1a00000, 0xe12fff30, 0xe12fff1f,
    }; /* MOV, BLX, BX PC */
    for (unsigned i = 0; i < sizeof(helper) / sizeof(helper[0]); ++i)
    {
        uint32_t words[8];
        memset(words, 0xa5, sizeof(words));
        gpsp_rv32_code code;
        gpsp_rv32_code_init(&code, words, sizeof(words));
        CHECK(gpsp_rv32_emit_arm_branch(&code, helper[i], 0x08000000) ==
                GPSP_RV32_ARM_BRANCH_NEEDS_HELPER);
        CHECK(code.ptr == code.start);
        for (unsigned j = 0; j < 8; ++j) CHECK(words[j] == 0xa5a5a5a5);
    }

    uint32_t words[8];
    memset(words, 0xa5, sizeof(words));
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, 4);
    CHECK(gpsp_rv32_emit_arm_branch(&code, 0xeb000002, 0x08000000) ==
            GPSP_RV32_ARM_BRANCH_BUFFER_FULL);
    CHECK(code.ptr == code.start && words[0] == 0xa5a5a5a5);

    gpsp_rv32_arm_condition_skip skip;
    gpsp_rv32_code_init(&code, words, 4);
    CHECK(!gpsp_rv32_emit_arm_condition_skip(&code, 8, &skip));
    CHECK(code.ptr == code.start && words[0] == 0xa5a5a5a5);
}

static void test_direct_condition_words()
{
    static const unsigned expected[] = {
        1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 2, 2, 0,
    };
    for (unsigned cond = 0; cond < 15; ++cond)
    {
        uint32_t words[8] = {0};
        gpsp_rv32_code code;
        gpsp_rv32_code_init(&code, words, sizeof(words));
        gpsp_rv32_arm_condition_skip skip;
        CHECK(gpsp_rv32_emit_arm_condition_skip(&code, cond, &skip));
        CHECK((size_t)(code.ptr - code.start) == expected[cond] * 4);
        CHECK(gpsp_rv32_patch_arm_condition_skip(&code, &skip, code.ptr));
        for (unsigned i = 0; i < expected[cond]; ++i)
            CHECK((words[i] & 0x7f) == 0x63);
    }

    uint32_t words[16];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(gpsp_rv32_emit_arm_branch(&code, 0x0a000002, 0x08000100) ==
            GPSP_RV32_ARM_BRANCH_EXIT);
    CHECK((size_t)(code.ptr - code.start) == 6 * 4); /* BEQ */
    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(gpsp_rv32_emit_arm_branch(&code, 0x8a000002, 0x08000100) ==
            GPSP_RV32_ARM_BRANCH_EXIT);
    CHECK((size_t)(code.ptr - code.start) == 7 * 4); /* BHI */
    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(gpsp_rv32_emit_arm_branch(&code, 0xaa000002, 0x08000100) ==
            GPSP_RV32_ARM_BRANCH_EXIT);
    CHECK((size_t)(code.ptr - code.start) == 6 * 4); /* BGE */
    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(gpsp_rv32_emit_arm_branch(&code, 0xca000002, 0x08000100) ==
            GPSP_RV32_ARM_BRANCH_EXIT);
    CHECK((size_t)(code.ptr - code.start) == 7 * 4); /* BGT */
    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(gpsp_rv32_emit_arm_branch_taken(&code, 0x0a000002,
            0x08000100) == GPSP_RV32_ARM_BRANCH_EXIT);
    CHECK((size_t)(code.ptr - code.start) == 2 * 4);
    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(gpsp_rv32_emit_arm_branch_taken(&code, 0x012fff10,
            0x08000100) == GPSP_RV32_ARM_BRANCH_EXIT);
    CHECK((size_t)(code.ptr - code.start) == 2 * 4);
}

int main(void)
{
    /* Exact sizes below assume 32-bit instructions. */
    gpsp_rv32_test_compress = false;
    test_conditions_and_link();
    test_displacements();
    test_bx();
    test_helper_and_buffer_rollback();
    test_direct_condition_words();
    puts("ARM RV32 branch tests: PASS");
    return 0;
}
