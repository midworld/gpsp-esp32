/* Thumb-to-RV32 single-instruction tests with a small RV32I/M executor.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rv32_thumb_alu.h"
#include "rv32_register_map.h"

#include <limits.h>
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

enum { N = RV_FLAG_N, Z = RV_FLAG_Z, C = RV_FLAG_C, V = RV_FLAG_V };
#define mapped guest_reg_map

static void test_fixed_register_map(void)
{
    static const uint8_t expected[15] = {
        13, 30, 31, 16, 23, 24, 25, 26, 17, 5, 6, 7, 28, 27, 29,
    };
    for (unsigned i = 0; i < 15; ++i)
        CHECK(guest_reg_map[i] == expected[i]);
    for (unsigned i = 0; i < 15; ++i)
        CHECK(guest_reg_map[i] != RV_STATE);
}

static int32_t sign_extend(uint32_t value, unsigned bits)
{
    uint32_t sign = 1u << (bits - 1);
    return (int32_t)((value ^ sign) - sign);
}

static uint32_t rotate_right(uint32_t value, unsigned amount)
{
    return amount ? (value >> amount) | (value << (32 - amount)) : value;
}

static int32_t b_offset(uint32_t word)
{
    uint32_t value = ((word >> 31) << 12) | (((word >> 7) & 1) << 11) |
            (((word >> 25) & 0x3f) << 5) | (((word >> 8) & 15) << 1);
    return sign_extend(value, 13);
}

static int32_t j_offset(uint32_t word)
{
    uint32_t value = ((word >> 31) << 20) | (((word >> 12) & 0xff) << 12) |
            (((word >> 20) & 1) << 11) | (((word >> 21) & 0x3ff) << 1);
    return sign_extend(value, 21);
}

static void execute_rv32(const uint32_t *words, size_t size, uint32_t host[32])
{
    size_t pc = 0;
    unsigned steps = 0;
    while (pc < size)
    {
        CHECK(++steps < 200 && !(pc & 1));
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
            case 0x13: /* OP-IMM */
                switch (funct3)
                {
                    case 0: host[rd] = host[rs1] + (uint32_t)imm; break;
                    case 2: host[rd] = (int32_t)host[rs1] < imm; break;
                    case 3: host[rd] = host[rs1] < (uint32_t)imm; break;
                    case 4: host[rd] = host[rs1] ^ (uint32_t)imm; break;
                    case 6: host[rd] = host[rs1] | (uint32_t)imm; break;
                    case 7: host[rd] = host[rs1] & (uint32_t)imm; break;
                    case 1:
                        CHECK(funct7 == 0);
                        host[rd] = host[rs1] << rs2;
                        break;
                    case 5:
                        CHECK(funct7 == 0 || funct7 == 0x20 || funct7 == 0x24 ||
                                funct7 == 0x30);
                        host[rd] = funct7 == 0x30 ?
                                rotate_right(host[rs1], rs2) :
                                funct7 == 0x24 ? (host[rs1] >> rs2) & 1 :
                                funct7 ? (uint32_t)((int32_t)host[rs1] >> rs2) :
                                host[rs1] >> rs2;
                        break;
                    default: CHECK(0);
                }
                break;
            case 0x33: /* OP and MUL */
                switch (funct3)
                {
                    case 0:
                        if (funct7 == 1)
                            host[rd] = (uint32_t)((uint64_t)host[rs1] * host[rs2]);
                        else
                        {
                            CHECK(funct7 == 0 || funct7 == 0x20);
                            host[rd] = funct7 ? host[rs1] - host[rs2] :
                                    host[rs1] + host[rs2];
                        }
                        break;
                    case 1: CHECK(funct7 == 0); host[rd] = host[rs1] << (host[rs2] & 31); break;
                    case 2:
                        CHECK(funct7 == 0);
                        host[rd] = (int32_t)host[rs1] < (int32_t)host[rs2];
                        break;
                    case 3: CHECK(funct7 == 0); host[rd] = host[rs1] < host[rs2]; break;
                    case 4: CHECK(funct7 == 0); host[rd] = host[rs1] ^ host[rs2]; break;
                    case 5:
                        CHECK(funct7 == 0 || funct7 == 0x20 || funct7 == 0x24 ||
                                funct7 == 0x30);
                        host[rd] = funct7 == 0x30 ?
                                rotate_right(host[rs1], host[rs2] & 31) :
                                funct7 == 0x24 ? (host[rs1] >> (host[rs2] & 31)) & 1 :
                                funct7 ? (uint32_t)((int32_t)host[rs1] >> (host[rs2] & 31)) :
                                host[rs1] >> (host[rs2] & 31);
                        break;
                    case 6: CHECK(funct7 == 0); host[rd] = host[rs1] | host[rs2]; break;
                    case 7:
#if GPSP_RV32_USE_ZBB
                        CHECK(funct7 == 0 || funct7 == 0x20);
                        host[rd] = host[rs1] & (funct7 ? ~host[rs2] : host[rs2]);
#else
                        CHECK(funct7 == 0);
                        host[rd] = host[rs1] & host[rs2];
#endif
                        break;
                    default: CHECK(0);
                }
                break;
            case 0x37: host[rd] = word & 0xfffff000; break; /* LUI */
            case 0x63: /* branch */
            {
                bool take = funct3 == 0 ? host[rs1] == host[rs2] :
                        funct3 == 1 ? host[rs1] != host[rs2] : false;
                CHECK(funct3 == 0 || funct3 == 1);
                if (take)
                {
                    int64_t target = (int64_t)pc + b_offset(word);
                    CHECK(target >= 0 && target <= (int64_t)size);
                    next = (size_t)target;
                }
                break;
            }
            case 0x6f: /* JAL */
            {
                int64_t target = (int64_t)pc + j_offset(word);
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

static gpsp_rv32_thumb_result run_flags(uint16_t opcode, uint32_t pc,
        uint8_t flags, uint32_t host[32])
{
    uint32_t words[128];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    gpsp_rv32_thumb_result result = gpsp_rv32_emit_thumb(&code, opcode,
            pc, flags);
    CHECK(result == GPSP_RV32_THUMB_EMITTED ||
            result == GPSP_RV32_THUMB_EXIT);
    execute_rv32(words, (size_t)(code.ptr - code.start), host);
    return result;
}

static gpsp_rv32_thumb_result run(uint16_t opcode, uint32_t pc,
        uint32_t host[32])
{
    return run_flags(opcode, pc, 15, host);
}

static size_t emitted_size(uint16_t opcode, uint8_t flags)
{
    uint32_t words[128];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(gpsp_rv32_emit_thumb(&code, opcode, 0x08000000, flags) ==
            GPSP_RV32_THUMB_EMITTED);
    return (size_t)(code.ptr - code.start);
}

static uint32_t emitted_word(uint16_t opcode)
{
    uint32_t words[128];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(gpsp_rv32_emit_thumb(&code, opcode, 0x08000000, 0) ==
            GPSP_RV32_THUMB_EMITTED);
    CHECK(code.ptr - code.start == 4);
    return words[0];
}

static void expect_nzcv(const uint32_t host[32], unsigned n, unsigned z,
        unsigned c, unsigned v)
{
    CHECK(host[N] == n && host[Z] == z && host[C] == c && host[V] == v);
}

static void test_alu()
{
    uint32_t host[32] = {0};
    host[mapped[1]] = 0x7fffffff;
    host[mapped[2]] = 1;
    CHECK(run(0x1888, 0x08000000, host) == GPSP_RV32_THUMB_EMITTED);
    CHECK(host[mapped[0]] == 0x80000000);
    expect_nzcv(host, 1, 0, 0, 1);

    memset(host, 0, sizeof(host));
    host[mapped[1]] = 0;
    host[mapped[2]] = 1;
    run(0x1a88, 0x08000000, host); /* SUB r0,r1,r2 */
    CHECK(host[mapped[0]] == 0xffffffff);
    expect_nzcv(host, 1, 0, 0, 0);

    memset(host, 0, sizeof(host));
    host[C] = 1;
    host[V] = 1;
    run(0x2000, 0x08000000, host); /* MOVS r0,#0 */
    CHECK(host[mapped[0]] == 0);
    expect_nzcv(host, 0, 1, 1, 1);

    memset(host, 0, sizeof(host));
    host[mapped[0]] = 0;
    run(0x2801, 0x08000000, host); /* CMP r0,#1 */
    CHECK(host[mapped[0]] == 0);
    expect_nzcv(host, 1, 0, 0, 0);

    memset(host, 0, sizeof(host));
    host[mapped[0]] = 0xffffffff;
    host[mapped[1]] = 0;
    host[C] = 1;
    run(0x4148, 0x08000000, host); /* ADC r0,r1 */
    CHECK(host[mapped[0]] == 0);
    expect_nzcv(host, 0, 1, 1, 0);

    memset(host, 0, sizeof(host));
    run(0x4188, 0x08000000, host); /* SBC r0,r1, old C=0 */
    CHECK(host[mapped[0]] == 0xffffffff);
    expect_nzcv(host, 1, 0, 0, 0);

    memset(host, 0, sizeof(host));
    host[mapped[0]] = 0x80000000;
    host[mapped[1]] = 0xffffffff;
    host[C] = 1;
    host[V] = 1;
    run(0x4348, 0x08000000, host); /* MUL r0,r1 */
    CHECK(host[mapped[0]] == 0x80000000);
    expect_nzcv(host, 1, 0, 1, 1);

    memset(host, 0, sizeof(host));
    host[mapped[0]] = 0x80000000;
    host[mapped[1]] = 0xffffffff;
    run(0x4208, 0x08000000, host); /* TST r0,r1 */
    CHECK(host[mapped[0]] == 0x80000000);
    expect_nzcv(host, 1, 0, 0, 0);
}

static void test_arithmetic_boundaries()
{
    CHECK(emitted_size(0x1888, 15) == 32);
    static const uint32_t values[] = {
        0, 1, 0x7fffffff, 0x80000000, 0xffffffff,
    };
    for (unsigned operation = 0; operation < 4; ++operation)
        for (unsigned carry_in = 0; carry_in < 2; ++carry_in)
            for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
                for (unsigned j = 0; j < sizeof(values) / sizeof(values[0]); ++j)
                {
                    uint32_t a = values[i], b = values[j];
                    uint32_t host[32] = {0};
                    uint16_t opcode = operation == 0 ? 0x1888 :
                            operation == 1 ? 0x1a88 :
                            operation == 2 ? 0x4148 : 0x4188;
                    if (operation < 2)
                    {
                        host[mapped[1]] = a;
                        host[mapped[2]] = b;
                    }
                    else
                    {
                        host[mapped[0]] = a;
                        host[mapped[1]] = b;
                    }
                    host[C] = carry_in;
                    run(opcode, 0x08000000, host);

                    bool subtract = operation == 1 || operation == 3;
                    uint32_t cin = operation == 2 ? carry_in : 0;
                    uint32_t borrow = operation == 3 ? 1 - carry_in : 0;
                    uint32_t result = subtract ? a - b - borrow : a + b + cin;
                    uint64_t wide = subtract ? (uint64_t)a - b - borrow :
                            (uint64_t)a + b + cin;
                    unsigned carry = subtract ?
                            (uint64_t)a >= (uint64_t)b + borrow : wide > UINT32_MAX;
                    int64_t signed_wide = subtract ?
                            (int64_t)(int32_t)a - (int64_t)(int32_t)b - borrow :
                            (int64_t)(int32_t)a + (int64_t)(int32_t)b + cin;
                    unsigned overflow = signed_wide < INT32_MIN ||
                            signed_wide > INT32_MAX;
                    CHECK(host[mapped[0]] == result);
                    expect_nzcv(host, result >> 31, result == 0, carry, overflow);
                }
}

static void check_arithmetic_alias(uint16_t opcode, unsigned rd,
        unsigned lhs_index, unsigned rhs_index, bool subtract, bool with_carry,
        bool write_result, unsigned carry_in, const uint32_t values[3])
{
    uint32_t a = lhs_index == 3 ? 0 : values[lhs_index];
    uint32_t b = values[rhs_index];
    unsigned cin = !subtract && with_carry ? carry_in : 0;
    unsigned borrow = subtract && with_carry ? 1 - carry_in : 0;
    uint32_t result = subtract ? a - b - borrow : a + b + cin;
    unsigned carry = subtract ?
            (uint64_t)a >= (uint64_t)b + borrow :
            (uint64_t)a + b + cin > UINT32_MAX;
    int64_t signed_result = subtract ?
            (int64_t)(int32_t)a - (int64_t)(int32_t)b - borrow :
            (int64_t)(int32_t)a + (int64_t)(int32_t)b + cin;
    unsigned overflow = signed_result < INT32_MIN || signed_result > INT32_MAX;

    for (unsigned mask = 0; mask < 16; mask++)
    {
        uint32_t host[32] = {0};
        for (unsigned i = 0; i < 3; i++)
            host[mapped[i]] = values[i];
        host[N] = host[Z] = host[V] = 1;
        host[C] = carry_in;
        run_flags(opcode, 0x08000000, mask, host);
        for (unsigned i = 0; i < 3; i++)
            CHECK(host[mapped[i]] ==
                    (write_result && i == rd ? result : values[i]));
        expect_nzcv(host, (mask & 8) ? result >> 31 : 1,
                (mask & 4) ? result == 0 : 1,
                (mask & 2) ? carry : carry_in,
                (mask & 1) ? overflow : 1);
    }
}

static void test_arithmetic_aliases(void)
{
    static const uint32_t values[][3] = {
        {0x7fffffff, 1, 0x80000000},
        {0xffffffff, 0, 1},
        {0x80000000, 0xffffffff, 0},
    };
    for (unsigned v = 0; v < sizeof(values) / sizeof(values[0]); v++)
        for (unsigned carry = 0; carry < 2; carry++)
        {
            for (unsigned rd = 0; rd < 3; rd++)
                for (unsigned rs = 0; rs < 3; rs++)
                {
                    for (unsigned operand = 0; operand < 3; operand++)
                        for (unsigned subtract = 0; subtract < 2; subtract++)
                            check_arithmetic_alias((uint16_t)(0x1800 |
                                    (subtract << 9) | (operand << 6) | (rs << 3) | rd),
                                    rd, rs, operand, subtract, false, true, carry, values[v]);
                    for (unsigned operation = 5; operation <= 11; operation++)
                    {
                        if (operation == 7 || operation == 8)
                            continue;
                        check_arithmetic_alias((uint16_t)(0x4000 |
                                (operation << 6) | (rs << 3) | rd), rd,
                                operation == 9 ? 3 : rd, rs,
                                operation == 6 || operation == 9 || operation == 10,
                                operation == 5 || operation == 6,
                                operation != 10 && operation != 11, carry, values[v]);
                    }
                }
        }

    CHECK(emitted_size(0x1888, 0) == 4);  /* ADD r0,r1,r2 */
    CHECK(emitted_size(0x1a88, 12) == 12); /* SUB with N/Z */
    CHECK(emitted_size(0x4148, 2) == 24); /* ADC with carry output */
    CHECK(emitted_size(0x4248, 0) == 4);  /* NEG r0,r1 */
    CHECK(emitted_size(0x4208, 3) == 0); /* TST with dead N/Z */
}

static void test_logic_and_negate()
{
    static const struct { uint16_t opcode; uint32_t result;
            unsigned n, z, c, v; } cases[] = {
        {0x4008, 1, 0, 0, 1, 1},          /* AND */
        {0x4048, 0x80000000, 1, 0, 1, 1}, /* EOR */
        {0x4208, 0x80000001, 0, 0, 1, 1},/* TST */
        {0x4248, 0xffffffff, 1, 0, 0, 0}, /* NEG */
        {0x4288, 0x80000001, 1, 0, 1, 0},/* CMP */
        {0x42c8, 0x80000001, 1, 0, 0, 0},/* CMN */
        {0x4308, 0x80000001, 1, 0, 1, 1}, /* ORR */
        {0x4348, 0x80000001, 1, 0, 1, 1}, /* MUL */
        {0x4388, 0x80000000, 1, 0, 1, 1}, /* BIC */
        {0x43c8, 0xfffffffe, 1, 0, 1, 1}, /* MVN */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        uint32_t host[32] = {0};
        host[mapped[0]] = 0x80000001;
        host[mapped[1]] = 1;
        host[C] = 1;
        host[V] = 1;
        run(cases[i].opcode, 0x08000000, host);
        CHECK(host[mapped[0]] == cases[i].result);
        expect_nzcv(host, cases[i].n, cases[i].z,
                cases[i].c, cases[i].v);
    }
}

static void test_direct_alu()
{
    static const struct { uint16_t opcode; uint32_t result; } cases[] = {
        {0x4000, 0x80000001}, /* AND r0,r0 */
        {0x4040, 0},          /* EOR r0,r0 */
        {0x4300, 0x80000001}, /* ORR r0,r0 */
        {0x4340, 1},          /* MUL r0,r0 */
        {0x4380, 0},          /* BIC r0,r0 */
        {0x43c0, 0x7ffffffe}, /* MVN r0,r0 */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        CHECK(emitted_size(cases[i].opcode, 0) ==
                (cases[i].opcode == 0x4000 || cases[i].opcode == 0x4300 ? 0 : 4));
        for (unsigned mask = 0; mask < 16; ++mask)
        {
            uint32_t host[32] = {0};
            host[mapped[0]] = 0x80000001;
            host[Z] = host[C] = host[V] = 1;
            run_flags(cases[i].opcode, 0x08000000, mask, host);
            CHECK(host[mapped[0]] == cases[i].result);
            expect_nzcv(host,
                    (mask & 8) ? cases[i].result >> 31 : 0,
                    (mask & 4) ? cases[i].result == 0 : 1, 1, 1);
        }
    }

    uint32_t word = emitted_word(0x4008); /* AND r0,r1 */
    CHECK((word & 0x7f) == 0x33 && ((word >> 12) & 7) == 7);
    CHECK(((word >> 7) & 31) == mapped[0]);
    CHECK(((word >> 15) & 31) == mapped[0]);
    CHECK(((word >> 20) & 31) == mapped[1]);
}

static void test_direct_immediates()
{
    static const struct {
        uint16_t opcode;
        unsigned dest, source;
        int immediate;
    } shape[] = {
        {0x20ff, 0, X0, 255}, /* MOV r0,#255 */
        {0x3001, 0, mapped[0], 1}, /* ADD r0,#1 */
        {0x3801, 0, mapped[0], -1}, /* SUB r0,#1 */
        {0x1dc8, 0, mapped[1], 7}, /* ADD r0,r1,#7 */
        {0x1fc8, 0, mapped[1], -7}, /* SUB r0,r1,#7 */
        {0xa901, 1, mapped[13], 4}, /* ADD r1,SP,#4 */
    };
    for (unsigned i = 0; i < sizeof(shape) / sizeof(shape[0]); ++i)
    {
        uint32_t word = emitted_word(shape[i].opcode);
        CHECK((word & 0x7f) == 0x13 && ((word >> 12) & 7) == 0);
        CHECK(((word >> 7) & 31) == mapped[shape[i].dest]);
        CHECK(((word >> 15) & 31) == shape[i].source);
        CHECK(sign_extend(word >> 20, 12) == shape[i].immediate);
    }

    static const struct {
        uint16_t opcode;
        uint32_t source, result;
        unsigned carry, overflow;
    } arithmetic[] = {
        {0x3001, 0x7fffffff, 0x80000000, 0, 1},
        {0x3801, 0, 0xffffffff, 0, 0},
        {0x1c48, 0x7fffffff, 0x80000000, 0, 1},
        {0x1e48, 0, 0xffffffff, 0, 0},
    };
    for (unsigned i = 0; i < sizeof(arithmetic) / sizeof(arithmetic[0]); ++i)
        for (unsigned mask = 0; mask < 16; ++mask)
        {
            uint32_t host[32] = {0};
            unsigned source = i < 2 ? 0 : 1;
            host[mapped[source]] = arithmetic[i].source;
            host[Z] = host[C] = host[V] = 1;
            run_flags(arithmetic[i].opcode, 0x08000000, mask, host);
            CHECK(host[mapped[0]] == arithmetic[i].result);
            if (source)
                CHECK(host[mapped[1]] == arithmetic[i].source);
            expect_nzcv(host, (mask & 8) ? 1 : 0, (mask & 4) ? 0 : 1,
                    (mask & 2) ? arithmetic[i].carry : 1,
                    (mask & 1) ? arithmetic[i].overflow : 1);
        }

    uint32_t host[32] = {0};
    host[C] = host[V] = 1;
    run(0x2000, 0x08000000, host); /* MOV r0,#0 preserves C/V. */
    CHECK(host[mapped[0]] == 0);
    expect_nzcv(host, 0, 1, 1, 1);
    CHECK(emitted_size(0x2000, 15) == 12);
    CHECK(emitted_size(0x3001, 12) == 12);
    CHECK(emitted_size(0x3801, 12) == 12);
    CHECK(emitted_size(0x1c48, 12) == 12);
    CHECK(emitted_size(0x1e48, 12) == 12);
    CHECK(emitted_size(0x3000, 0) == 0);  /* ADD r0,#0 */
    CHECK(emitted_size(0x3800, 0) == 0);  /* SUB r0,#0 */
    CHECK(emitted_size(0x1c00, 0) == 0);  /* ADD r0,r0,#0 */
    CHECK(emitted_size(0x1c08, 0) == 4);  /* ADD r0,r1,#0 */
    CHECK(emitted_size(0x3000, 15) == 16);
    CHECK(emitted_size(0x3800, 15) == 16);
    CHECK(emitted_size(0x2800, 15) == 16); /* CMP r0,#0 */
}

static void test_immediate_arithmetic_flags(void)
{
    static const uint32_t values[] = {
        0, 1, 0x7fffff00, 0x7fffffff, 0x80000000,
        0xffffff00, 0xffffffff,
    };
    static const unsigned immediates[] = {0, 1, 7, 127, 255};

    CHECK(emitted_size(0x30ff, 15) == 24);
    CHECK(emitted_size(0x38ff, 15) == 32);
    CHECK(emitted_size(0x28ff, 15) == 28);
    CHECK(emitted_size(0x1dc8, 15) == 20);
    CHECK(emitted_size(0x1fc8, 15) == 28);

    for (unsigned form = 0; form < 5; form++)
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++)
            for (unsigned j = 0; j < sizeof(immediates) / sizeof(immediates[0]); j++)
            {
                unsigned immediate = immediates[j];
                if (form >= 3 && immediate > 7)
                    continue;
                bool subtract = form == 1 || form == 2 || form == 4;
                bool compare = form == 2;
                unsigned source = form >= 3 ? 1 : 0;
                uint16_t opcode = form == 0 ? 0x3000 | immediate :
                        form == 1 ? 0x3800 | immediate :
                        form == 2 ? 0x2800 | immediate :
                        (form == 3 ? 0x1c08 : 0x1e08) | (immediate << 6);
                uint32_t input = values[i];
                uint32_t result = subtract ? input - immediate : input + immediate;
                unsigned carry = subtract ? input >= immediate : result < immediate;
                int64_t signed_result = subtract ?
                        (int64_t)(int32_t)input - immediate :
                        (int64_t)(int32_t)input + immediate;
                unsigned overflow = signed_result < INT32_MIN ||
                        signed_result > INT32_MAX;

                for (unsigned mask = 0; mask < 16; mask++)
                {
                    uint32_t host[32] = {0};
                    host[mapped[source]] = input;
                    host[mapped[0]] = source ? 0x13579bdf : input;
                    host[N] = 0;
                    host[Z] = 1;
                    host[C] = 0;
                    host[V] = 1;
                    run_flags(opcode, 0x08000000, mask, host);
                    CHECK(host[mapped[0]] == (compare ? input : result));
                    if (source)
                        CHECK(host[mapped[source]] == input);
                    expect_nzcv(host, (mask & 8) ? result >> 31 : 0,
                            (mask & 4) ? result == 0 : 1,
                            (mask & 2) ? carry : 0,
                            (mask & 1) ? overflow : 1);
                }
            }
}

static void test_flag_liveness(void)
{
    static const struct {
        uint16_t opcode;
        uint32_t a, b, result;
        unsigned n, z, c, v;
        bool carry_input;
    } cases[] = {
        {0x1888, 0x7fffffff, 1, 0x80000000, 1, 0, 0, 1, false},
        {0x4148, 0xffffffff, 1, 0, 0, 1, 1, 0, true},
        {0x4188, 1, 0, 0, 0, 1, 1, 0, true},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        for (unsigned mask = 0; mask < 16; ++mask)
        {
            uint32_t host[32] = {0};
            if (cases[i].carry_input)
            {
                host[mapped[0]] = cases[i].a;
                host[mapped[1]] = cases[i].b;
            }
            else
            {
                host[mapped[1]] = cases[i].a;
                host[mapped[2]] = cases[i].b;
            }
            host[N] = 0;
            host[Z] = 0;
            host[C] = cases[i].carry_input ? 0 : 1;
            host[V] = 1;
            unsigned old_n = host[N], old_z = host[Z];
            unsigned old_c = host[C], old_v = host[V];
            run_flags(cases[i].opcode, 0x08000000, mask, host);
            CHECK(host[mapped[0]] == cases[i].result);
            expect_nzcv(host,
                    (mask & 8) ? cases[i].n : old_n,
                    (mask & 4) ? cases[i].z : old_z,
                    (mask & 2) ? cases[i].c : old_c,
                    (mask & 1) ? cases[i].v : old_v);
        }

    uint32_t host[32] = {0};
    host[mapped[0]] = 0x80000001;
    host[mapped[1]] = 32;
    host[C] = 0;
    run_flags(0x41c8, 0x08000000, 12, host); /* ROR r0,r1 */
    CHECK(host[mapped[0]] == 0x80000001);
    expect_nzcv(host, 1, 0, 0, 0);

    host[mapped[1]] = 1;
    run_flags(0x0008, 0x08000000, 12, host); /* LSL r0,r1,#0 */
    CHECK(host[mapped[0]] == 1);
    expect_nzcv(host, 0, 0, 0, 0);

    CHECK(emitted_size(0x1888, 0) < emitted_size(0x1888, 15));
    CHECK(emitted_size(0x41c8, 12) < emitted_size(0x41c8, 15));
    CHECK(emitted_size(0x2801, 0) == 0); /* Dead CMP has no guest effect. */
    CHECK(emitted_size(0x4208, 0) == 0); /* Dead TST has no guest effect. */
}

static void test_shift_flag_liveness(void)
{
    static const uint16_t opcodes[] = {0x4088, 0x40c8, 0x4108, 0x41c8};
    static const uint32_t counts[] = {0, 1, 31, 32, 33, 255, 256, 257};
    static const uint8_t masks[] = {0, 4, 8, 12};
    for (unsigned kind = 0; kind < 4; ++kind)
        for (unsigned count = 0; count < sizeof(counts) / sizeof(counts[0]);
                ++count)
        {
            uint32_t full[32] = {0};
            full[mapped[0]] = 0x80000001;
            full[mapped[1]] = counts[count];
            full[N] = 0;
            full[Z] = 1;
            full[C] = 0;
            full[V] = 1;
            run(opcodes[kind], 0x08000000, full);
            for (unsigned m = 0; m < sizeof(masks) / sizeof(masks[0]); ++m)
            {
                uint32_t host[32] = {0};
                host[mapped[0]] = 0x80000001;
                host[mapped[1]] = counts[count];
                host[N] = 0;
                host[Z] = 1;
                host[C] = 0;
                host[V] = 1;
                run_flags(opcodes[kind], 0x08000000, masks[m], host);
                CHECK(host[mapped[0]] == full[mapped[0]]);
                expect_nzcv(host, (masks[m] & 8) ? full[N] : 0,
                        (masks[m] & 4) ? full[Z] : 1, 0, 1);
            }
        }
}

static void test_shift_boundaries()
{
    CHECK(emitted_size(0x0048, 0) == 4); /* LSL r0,r1,#1 */
    CHECK(emitted_size(0x0808, 0) == 4); /* LSR r0,r1,#32 */
    CHECK(emitted_size(0x1008, 0) == 4); /* ASR r0,r1,#32 */
    uint32_t direct[32] = {0};
    direct[mapped[1]] = 0x80000001;
    direct[C] = direct[V] = 1;
    run_flags(0x0048, 0x08000000, 0, direct);
    CHECK(direct[mapped[0]] == 2);
    run_flags(0x0808, 0x08000000, 0, direct);
    CHECK(direct[mapped[0]] == 0);
    run_flags(0x1008, 0x08000000, 0, direct);
    CHECK(direct[mapped[0]] == 0xffffffff);
    direct[mapped[0]] = 3;
    run_flags(0x0040, 0x08000000, 0, direct); /* LSL r0,r0,#1 */
    CHECK(direct[mapped[0]] == 6);
    expect_nzcv(direct, 0, 0, 1, 1);

    static const unsigned counts[] = {0, 1, 31, 32, 33, 255, 256, 257};
    for (unsigned kind = 0; kind < 4; ++kind)
        for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i)
        {
            uint32_t host[32] = {0};
            host[mapped[0]] = 0x80000001;
            host[mapped[1]] = counts[i];
            host[C] = 0;
            uint16_t opcode = 0x4000 | ((kind == 3 ? 7 : kind + 2) << 6) | (1 << 3);
            run(opcode, 0x08000000, host);
            uint32_t expected = 0x80000001;
            unsigned carry = 0;
            unsigned count = counts[i] & 255;
            if (count)
            {
                if (kind == 0) /* LSL */
                {
                    if (count < 32)
                    {
                        carry = (expected >> (32 - count)) & 1;
                        expected <<= count;
                    }
                    else
                    {
                        carry = count == 32 ? expected & 1 : 0;
                        expected = 0;
                    }
                }
                else if (kind == 1) /* LSR */
                {
                    if (count < 32)
                    {
                        carry = (expected >> (count - 1)) & 1;
                        expected >>= count;
                    }
                    else
                    {
                        carry = count == 32 ? expected >> 31 : 0;
                        expected = 0;
                    }
                }
                else if (kind == 2) /* ASR */
                {
                    carry = count < 32 ? (expected >> (count - 1)) & 1 : expected >> 31;
                    expected = (uint32_t)((int32_t)expected >> (count < 32 ? count : 31));
                }
                else /* ROR */
                {
                    unsigned rotation = count & 31;
                    if (rotation)
                        expected = (expected >> rotation) |
                                (expected << (32 - rotation));
                    carry = expected >> 31;
                }
            }
            CHECK(host[mapped[0]] == expected);
            expect_nzcv(host, expected >> 31, expected == 0, carry, 0);
        }

    uint32_t host[32] = {0};
    host[mapped[1]] = 0x80000001;
    host[C] = 1;
    run(0x0008, 0x08000000, host); /* LSL r0,r1,#0 */
    CHECK(host[mapped[0]] == 0x80000001);
    expect_nzcv(host, 1, 0, 1, 0);
    run(0x0808, 0x08000000, host); /* LSR r0,r1,#0 => 32 */
    CHECK(host[mapped[0]] == 0);
    expect_nzcv(host, 0, 1, 1, 0);
    run(0x1008, 0x08000000, host); /* ASR r0,r1,#0 => 32 */
    CHECK(host[mapped[0]] == 0xffffffff);
    expect_nzcv(host, 1, 0, 1, 0);

    for (unsigned kind = 0; kind < 4; ++kind)
    {
        memset(host, 0, sizeof(host));
        host[mapped[0]] = 1;
        host[mapped[1]] = 0x100;
        host[C] = 1;
        uint16_t opcode = 0x4000 | ((kind == 3 ? 7 : kind + 2) << 6) | (1 << 3);
        run(opcode, 0x08000000, host); /* Rs[7:0] is zero. */
        CHECK(host[mapped[0]] == 1);
        expect_nzcv(host, 0, 0, 1, 0);
    }
}

static void test_shift_aliases(void)
{
    static const uint32_t values[] = {
        0, 1, 31, 32, 33, 255, 256, 257, 0x80000001, 0xffffffff,
    };
    static const unsigned amounts[] = {0, 1, 31};
    static const unsigned reg_operations[] = {2, 3, 4, 7};

    for (unsigned kind = 0; kind < 3; kind++)
        for (unsigned amount = 0; amount < sizeof(amounts) / sizeof(amounts[0]);
                amount++)
            for (unsigned value = 0; value < sizeof(values) / sizeof(values[0]);
                    value++)
                for (unsigned mask = 0; mask < 16; mask++)
                {
                    uint16_t alias_opcode = (uint16_t)((kind << 11) |
                            (amounts[amount] << 6));
                    uint16_t reference_opcode = alias_opcode | (1 << 3);
                    uint32_t alias[32] = {0}, reference[32] = {0};
                    alias[mapped[0]] = values[value];
                    reference[mapped[1]] = values[value];
                    alias[N] = reference[N] = 1;
                    alias[Z] = reference[Z] = 1;
                    alias[C] = reference[C] = 1;
                    alias[V] = reference[V] = 1;
                    run_flags(alias_opcode, 0x08000000, mask, alias);
                    run_flags(reference_opcode, 0x08000000, mask, reference);
                    CHECK(alias[mapped[0]] == reference[mapped[0]]);
                    expect_nzcv(alias, reference[N], reference[Z],
                            reference[C], reference[V]);
                }

    for (unsigned kind = 0; kind < 4; kind++)
        for (unsigned value = 0; value < sizeof(values) / sizeof(values[0]);
                value++)
            for (unsigned mask = 0; mask < 16; mask++)
            {
                uint16_t alias_opcode = (uint16_t)(0x4000 |
                        (reg_operations[kind] << 6));
                uint16_t reference_opcode = alias_opcode | (1 << 3);
                uint32_t alias[32] = {0}, reference[32] = {0};
                alias[mapped[0]] = values[value];
                reference[mapped[0]] = reference[mapped[1]] = values[value];
                alias[N] = reference[N] = 1;
                alias[Z] = reference[Z] = 1;
                alias[C] = reference[C] = 1;
                alias[V] = reference[V] = 1;
                run_flags(alias_opcode, 0x08000000, mask, alias);
                run_flags(reference_opcode, 0x08000000, mask, reference);
                CHECK(alias[mapped[0]] == reference[mapped[0]]);
                expect_nzcv(alias, reference[N], reference[Z],
                        reference[C], reference[V]);
            }

    CHECK(emitted_size(0x0000, 15) == 8); /* LSL r0,r0,#0 */
    CHECK(emitted_size(0x0000, 1) == 0);  /* V is preserved. */
#if GPSP_RV32_USE_ZBB
    CHECK(emitted_size(0x41c8, 12) == 12); /* ROR without carry output. */
#endif
}

#if GPSP_RV32_USE_ZBB
static void test_zbb_andn_encoding(void)
{
    uint32_t word = emitted_word(0x4388); /* BIC r0,r1 */
    CHECK((word & 0xfe00707f) == ((0x20u << 25) | (7u << 12) | 0x33));
    CHECK(((word >> 7) & 31) == mapped[0]);
    CHECK(((word >> 15) & 31) == mapped[0]);
    CHECK(((word >> 20) & 31) == mapped[1]);
}

static void test_zbb_rotate_encoding(void)
{
    uint32_t storage[128];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, storage, sizeof(storage));
    CHECK(gpsp_rv32_emit_thumb(&code, 0x41c8, 0x08000000, 15) ==
            GPSP_RV32_THUMB_EMITTED); /* ROR r0,r1 */
    bool found = false;
    for (size_t i = 0; i < (size_t)(code.ptr - code.start) / 4; ++i)
        found |= (storage[i] & 0xfe00707f) ==
                ((0x30u << 25) | (5u << 12) | 0x33);
    CHECK(found);
}
#endif

static bool cond(unsigned code, unsigned n, unsigned z, unsigned c, unsigned v)
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
        default: CHECK(0); return false;
    }
}

static void check_exit_adapter(gpsp_rv32_thumb_result result,
        uint32_t host[32], uint32_t *cpsr)
{
    CHECK(result == GPSP_RV32_THUMB_EXIT);
    host[RV_PC] = host[A0];
    *cpsr = (*cpsr & ~0x20u) | (host[A1] ? 0x20u : 0);
}

static void test_branches()
{
    const uint32_t pc = 0x08000100;
    const uint8_t condition_bytes[14] = {
        4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 4, 16, 8,
    };
    for (unsigned condition = 0; condition < 14; ++condition)
        for (unsigned flags = 0; flags < 16; ++flags)
        {
            uint32_t host[32] = {0};
            host[N] = flags & 1;
            host[Z] = (flags >> 1) & 1;
            host[C] = (flags >> 2) & 1;
            host[V] = (flags >> 3) & 1;
            uint32_t selected[32];
            memcpy(selected, host, sizeof(selected));
            selected[A0] = 0x12345678;
            selected[A1] = 0x87654321;
            uint32_t words[4];
            gpsp_rv32_code code;
            gpsp_rv32_code_init(&code, words, sizeof(words));
            CHECK(gpsp_rv32_emit_thumb_condition(&code, condition));
            CHECK((size_t)(code.ptr - code.start) == condition_bytes[condition]);
            execute_rv32(words, condition_bytes[condition], selected);
            CHECK(selected[RV_TMP0] == cond(condition,
                    host[N], host[Z], host[C], host[V]));
            CHECK(selected[A0] == 0x12345678 && selected[A1] == 0x87654321);
            gpsp_rv32_thumb_result result = run(0xd0fe | (condition << 8), pc, host);
            CHECK(result == GPSP_RV32_THUMB_EXIT);
            CHECK(host[A0] == (cond(condition, host[N], host[Z], host[C], host[V]) ?
                    pc : pc + 2));
            CHECK(host[A1] == 1);
            uint32_t cpsr = 0x10;
            check_exit_adapter(result, host, &cpsr);
            CHECK(host[RV_PC] == host[A0] && (cpsr & 0x20));
        }

    uint32_t short_words[4] = {0};
    gpsp_rv32_code short_code;
    gpsp_rv32_code_init(&short_code, short_words, 12);
    CHECK(!gpsp_rv32_emit_thumb_condition(&short_code, 12));
    CHECK(short_code.ptr == short_code.start && short_words[0] == 0);
    CHECK(!gpsp_rv32_emit_thumb_condition(&short_code, 14));

    uint32_t host[32] = {0};
    CHECK(run(0xe002, pc, host) == GPSP_RV32_THUMB_EXIT);
    CHECK(host[A0] == pc + 8 && host[A1] == 1);
    CHECK(run(0xe7ff, pc, host) == GPSP_RV32_THUMB_EXIT);
    CHECK(host[A0] == pc + 2 && host[A1] == 1);
}

static void test_high_register_and_bl()
{
    const uint32_t pc = 0x08000100;
    uint32_t host[32] = {0};
    CHECK(emitted_size(0x4680, 15) == 4);  /* MOV r8,r0 */
    CHECK(emitted_size(0x4480, 15) == 4);  /* ADD r8,r0 */
    CHECK(emitted_size(0x4678, 15) == 8);  /* MOV r0,PC */
    CHECK(emitted_size(0x4478, 15) == 12); /* ADD r0,PC */
    host[mapped[0]] = 0x12345678;
    host[C] = 1;
    host[V] = 1;
    CHECK(run(0x4680, pc, host) == GPSP_RV32_THUMB_EMITTED); /* MOV r8,r0 */
    CHECK(host[mapped[8]] == 0x12345678);
    expect_nzcv(host, 0, 0, 1, 1);

    host[mapped[8]] = 0x7fffffff;
    host[mapped[0]] = 1;
    CHECK(run(0x4480, pc, host) == GPSP_RV32_THUMB_EMITTED); /* ADD r8,r0 */
    CHECK(host[mapped[8]] == 0x80000000);
    expect_nzcv(host, 0, 0, 1, 1);

    host[mapped[8]] = 3;
    CHECK(run(0x44c0, pc, host) == GPSP_RV32_THUMB_EMITTED); /* ADD r8,r8 */
    CHECK(host[mapped[8]] == 6);
    CHECK(run(0x46c0, pc, host) == GPSP_RV32_THUMB_EMITTED); /* MOV r8,r8 */
    CHECK(host[mapped[8]] == 6);
    CHECK(emitted_size(0x46c0, 15) == 0);

    host[mapped[0]] = 2;
    CHECK(run(0x4678, pc, host) == GPSP_RV32_THUMB_EMITTED); /* MOV r0,PC */
    CHECK(host[mapped[0]] == pc + 4);
    CHECK(run(0x4478, pc, host) == GPSP_RV32_THUMB_EMITTED); /* ADD r0,PC */
    CHECK(host[mapped[0]] == 2 * (pc + 4));

    /* Metroid Fusion uses MOV r12,r0 here. r12 must not share RV_STATE, which
     * holds the reg[] base across generated helper calls. */
    memset(host, 0, sizeof(host));
    host[RV_STATE] = 0xdecafbadu;
    host[mapped[0]] = 0x24;
    CHECK(run(0x4684, pc, host) == GPSP_RV32_THUMB_EMITTED);
    CHECK(host[mapped[12]] == 0x24 && host[RV_STATE] == 0xdecafbadu);

    memset(host, 0, sizeof(host));
    host[mapped[8]] = 0x08000201;
    CHECK(run(0x46c7, pc, host) == GPSP_RV32_THUMB_EXIT); /* MOV PC,r8 */
    CHECK(host[A0] == 0x08000200 && host[A1] == 1);

    memset(host, 0, sizeof(host));
    host[mapped[8]] = 8;
    CHECK(run(0x44c7, pc, host) == GPSP_RV32_THUMB_EXIT); /* ADD PC,r8 */
    CHECK(host[A0] == pc + 12 && host[A1] == 1);

    memset(host, 0, sizeof(host));
    host[mapped[8]] = pc + 4;
    CHECK(run(0x45c7, pc, host) == GPSP_RV32_THUMB_EMITTED); /* CMP PC,r8 */
    expect_nzcv(host, 0, 1, 1, 0);

    memset(host, 0, sizeof(host));
    host[mapped[8]] = 0x08000203;
    CHECK(run(0x4740, pc, host) == GPSP_RV32_THUMB_EXIT); /* BX r8 */
    CHECK(host[A0] == 0x08000202 && host[A1] == 1);
    host[mapped[8]] = 0x08000202;
    CHECK(run(0x4740, pc, host) == GPSP_RV32_THUMB_EXIT);
    CHECK(host[A0] == 0x08000202 && host[A1] == 0);
    uint32_t cpsr = 0x30;
    check_exit_adapter(GPSP_RV32_THUMB_EXIT, host, &cpsr);
    CHECK((cpsr & 0x20) == 0);

    memset(host, 0, sizeof(host));
    CHECK(run(0xf7ff, pc, host) == GPSP_RV32_THUMB_EMITTED);
    CHECK(host[mapped[14]] == pc + 4 - 4096);
    host[mapped[14]] = 0x08001000; /* second half can be entered alone */
    CHECK(run(0xffff, pc + 2, host) == GPSP_RV32_THUMB_EXIT);
    CHECK(host[A0] == 0x08001000 + 4094);
    CHECK(host[mapped[14]] == ((pc + 4) | 1));
    CHECK(host[A1] == 1);

    memset(host, 0, sizeof(host));
    host[mapped[14]] = 0x08000201; /* standalone second half with odd LR */
    CHECK(run(0xf800, pc, host) == GPSP_RV32_THUMB_EXIT);
    CHECK(host[A0] == 0x08000201);
    CHECK(host[mapped[14]] == ((pc + 2) | 1));
    CHECK(host[A1] == 1);
    cpsr = 0x30;
    check_exit_adapter(GPSP_RV32_THUMB_EXIT, host, &cpsr);
    CHECK(host[RV_PC] == 0x08000201 && (cpsr & 0x20));
}

static void test_address_arithmetic()
{
    uint32_t host[32] = {0};
    const uint32_t pc = 0x08000102;
    run(0xa001, pc, host); /* ADD r0,PC,#4: word-align PC+4 */
    CHECK(host[mapped[0]] == 0x08000108);
    host[mapped[13]] = 0x03007f00;
    run(0xa901, pc, host); /* ADD r1,SP,#4 */
    CHECK(host[mapped[1]] == 0x03007f04);
    run(0xb001, pc, host); /* ADD SP,#4 */
    CHECK(host[mapped[13]] == 0x03007f04);
    run(0xb081, pc, host); /* SUB SP,#4 */
    CHECK(host[mapped[13]] == 0x03007f00);
    CHECK(emitted_size(0xb000, 15) == 0);
    CHECK(emitted_size(0xb080, 15) == 0);
}

static void test_helper_and_rollback()
{
    static const uint16_t helper[] = {
        0x4800, 0x6000, 0xb400, 0xbd00, 0xc000, 0xde00, 0xdf00,
        0xe800, 0x4780, 0xb100,
    };
    for (unsigned i = 0; i < sizeof(helper) / sizeof(helper[0]); ++i)
    {
        uint32_t words[8];
        memset(words, 0xa5, sizeof(words));
        gpsp_rv32_code code;
        gpsp_rv32_code_init(&code, words, sizeof(words));
        CHECK(gpsp_rv32_emit_thumb(&code, helper[i], 0x08000000, 15) ==
                GPSP_RV32_THUMB_NEEDS_HELPER);
        CHECK(code.ptr == code.start);
        for (unsigned n = 0; n < 8; ++n) CHECK(words[n] == 0xa5a5a5a5);
    }

    uint32_t words[8];
    memset(words, 0xa5, sizeof(words));
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, 4);
    CHECK(gpsp_rv32_emit_thumb(&code, 0x1888, 0x08000000, 15) ==
            GPSP_RV32_THUMB_BUFFER_FULL);
    CHECK(code.ptr == code.start && words[0] == 0xa5a5a5a5);
}

static void test_all_opcode_classification()
{
    for (unsigned opcode = 0; opcode <= 0xffff; ++opcode)
    {
        uint32_t words[128];
        memset(words, 0xa5, sizeof(words));
        gpsp_rv32_code code;
        gpsp_rv32_code_init(&code, words, sizeof(words));
        gpsp_rv32_thumb_result result = gpsp_rv32_emit_thumb(&code,
                (uint16_t)opcode, 0x08000000, 15);
        CHECK(result != GPSP_RV32_THUMB_BUFFER_FULL);
        CHECK(gpsp_rv32_thumb_needs_helper((uint16_t)opcode) ==
                (result == GPSP_RV32_THUMB_NEEDS_HELPER));
        if (result == GPSP_RV32_THUMB_NEEDS_HELPER)
            CHECK(code.ptr == code.start && words[0] == 0xa5a5a5a5);
        else
        {
            unsigned rd = ((opcode >> 4) & 8) | (opcode & 7);
            unsigned rs = (opcode >> 3) & 15;
            bool same_register_move = (opcode & 0xfc00) == 0x4400 &&
                    ((opcode >> 8) & 3) == 2 && rd != 15 && rd == rs;
            bool zero_sp_adjust = (opcode & 0xff7f) == 0xb000;
            CHECK((code.ptr == code.start) ==
                    (same_register_move || zero_sp_adjust));
        }
    }
}

int main(void)
{
    /* Exact sizes below assume 32-bit instructions. */
    gpsp_rv32_test_compress = false;
    test_fixed_register_map();
    test_alu();
    test_arithmetic_boundaries();
    test_arithmetic_aliases();
    test_logic_and_negate();
    test_direct_alu();
    test_direct_immediates();
    test_immediate_arithmetic_flags();
    test_flag_liveness();
    test_shift_flag_liveness();
    test_shift_boundaries();
    test_shift_aliases();
#if GPSP_RV32_USE_ZBB
    test_zbb_andn_encoding();
    test_zbb_rotate_encoding();
#endif
    test_branches();
    test_high_register_and_bl();
    test_address_arithmetic();
    test_helper_and_rollback();
    test_all_opcode_classification();
    puts("Thumb RV32 ALU tests: PASS");
    return 0;
}
