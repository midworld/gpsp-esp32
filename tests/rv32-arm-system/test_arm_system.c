/* ARM system instruction emission and effect tests.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include the module to test private callbacks and emitted RV instructions. */
#include "../../riscv/rv32_arm_system.c"
#include "../rv32-rvc.h"

#define GUEST(n) guest_reg_map[n]

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #value); \
        exit(1); \
    } \
} while (0)

#define TEST_SPSR_BASE 0x2000u

u32 reg[64];
u32 reg_mode[7][7];
u32 spsr[6];
u8 ws_cyc_seq[16][2];
u8 ws_cyc_nseq[16][2];
u32 idle_loop_target_pc;
u32 irq_entry_cycles;

const u32 cpu_modes[16] = {
    MODE_USER, MODE_FIQ, MODE_IRQ, MODE_SUPERVISOR,
    MODE_INVALID, MODE_INVALID, MODE_INVALID, MODE_ABORT,
    MODE_INVALID, MODE_INVALID, MODE_INVALID, MODE_UNDEFINED,
    MODE_INVALID, MODE_INVALID, MODE_INVALID, MODE_SYSTEM
};
const u32 cpsr_masks[4][2] = {
    {0, 0}, {0x20, 0xef}, {0xf0000000, 0xf0000000},
    {0xf0000020, 0xf00000ef}
};
const u32 spsr_masks[4] = {0, 0xef, 0xf0000000, 0xf00000ef};

static unsigned read_count;
static unsigned write_count;
static u32 read_address;
static u32 write_address;
static u32 write_value;
static u32 memory_value;
static u8 write_alert;
static bool raise_irq;

void gpsp_rv32_call_helper(void) {}
void gpsp_rv32_system_exit_shared(void) { abort(); }

void set_cpu_mode(cpu_mode_type new_mode)
{
    cpu_mode_type old_mode = reg[CPU_MODE];
    if (old_mode == new_mode)
        return;
    REG_MODE(old_mode)[5] = reg[REG_SP];
    REG_MODE(old_mode)[6] = reg[REG_LR];
    reg[REG_SP] = REG_MODE(new_mode)[5];
    reg[REG_LR] = REG_MODE(new_mode)[6];
    reg[CPU_MODE] = new_mode;
}

u32 check_and_raise_interrupts(void)
{
    if (!raise_irq)
        return 0;
    raise_irq = false;
    REG_MODE(MODE_IRQ)[6] = reg[REG_PC] + 4;
    REG_SPSR(MODE_IRQ) = reg[REG_CPSR];
    reg[REG_CPSR] = 0xd2;
    reg[REG_PC] = 0x18;
    set_cpu_mode(MODE_IRQ);
    return 1;
}

u32 read_memory8(u32 address)
{
    read_count++;
    read_address = address;
    return memory_value & 0xff;
}

u32 read_memory32(u32 address)
{
    read_count++;
    read_address = address;
    unsigned shift = (address & 3) * 8;
    return shift ? (memory_value >> shift) |
            (memory_value << (32 - shift)) : memory_value;
}

cpu_alert_type write_memory8(u32 address, u8 value)
{
    write_count++;
    write_address = address;
    write_value = value;
    return write_alert;
}

cpu_alert_type write_memory32(u32 address, u32 value)
{
    write_count++;
    write_address = address;
    write_value = value;
    return write_alert;
}

static void reset_state(void)
{
    memset(reg, 0, sizeof(reg));
    memset(reg_mode, 0, sizeof(reg_mode));
    memset(spsr, 0, sizeof(spsr));
    memset(ws_cyc_seq, 0, sizeof(ws_cyc_seq));
    memset(ws_cyc_nseq, 0, sizeof(ws_cyc_nseq));
    read_count = write_count = 0;
    read_address = write_address = write_value = memory_value = 0;
    write_alert = 0;
    raise_irq = false;
    idle_loop_target_pc = 0xffffffffu;
    gpsp_rv32_arm_system_take_alert();
    reg[CPU_MODE] = MODE_SYSTEM;
    reg[REG_CPSR] = 0x1f;
    reg[REG_PC] = 0x08000000;
    ws_cyc_seq[8][1] = 2;
}

static int32_t sign_extend(uint32_t value, unsigned width)
{
    uint32_t sign = 1u << (width - 1);
    return (int32_t)((value ^ sign) - sign);
}

static void run_inline(const uint32_t *words, size_t bytes, uint32_t host[32])
{
    size_t pc = 0;
    while (pc < bytes)
    {
        uint32_t word = 0;
        unsigned length = rv32_rvc_fetch(words, bytes, pc, &word);
        CHECK(length);
        if (!length)
            return;
        unsigned op = word & 0x7f;
        unsigned rd = (word >> 7) & 31;
        unsigned fn = (word >> 12) & 7;
        unsigned rs1 = (word >> 15) & 31;
        unsigned rs2 = (word >> 20) & 31;
        int32_t imm = sign_extend(word >> 20, 12);
        size_t next = pc + length;
        if (op == 0x03)
        {
            u32 address = host[rs1] + (u32)imm;
            CHECK(fn == 2);
            if (address == 0x1040)
                host[rd] = reg[REG_CPSR];
            else if (address == 0x1044)
                host[rd] = reg[CPU_MODE];
            else
            {
                CHECK(address >= TEST_SPSR_BASE &&
                        address < TEST_SPSR_BASE + sizeof(spsr) && !(address & 3));
                host[rd] = spsr[(address - TEST_SPSR_BASE) / 4];
            }
        }
        else if (op == 0x23)
        {
            int32_t offset = sign_extend(((word >> 25) << 5) |
                    ((word >> 7) & 31), 12);
            u32 address = host[rs1] + (u32)offset;
            CHECK(fn == 2);
            if (address == 0x104c)
                reg[REG_BUS_VALUE] = host[rs2];
            else
            {
                CHECK(address >= TEST_SPSR_BASE &&
                        address < TEST_SPSR_BASE + sizeof(spsr) && !(address & 3));
                spsr[(address - TEST_SPSR_BASE) / 4] = host[rs2];
            }
        }
        else if (op == 0x37)
            host[rd] = word & 0xfffff000u;
        else if (op == 0x13)
        {
            if (fn == 0)
                host[rd] = host[rs1] + (u32)imm;
            else if (fn == 7)
                host[rd] = host[rs1] & (u32)imm;
            else if (fn == 4)
                host[rd] = host[rs1] ^ (u32)imm;
            else if (fn == 1)
                host[rd] = host[rs1] << rs2;
            else if (fn == 5)
            {
                unsigned funct7 = word >> 25;
                CHECK(funct7 == 0 || funct7 == 0x20 || funct7 == 0x24);
                host[rd] = funct7 == 0x24 ? (host[rs1] >> rs2) & 1 :
                        funct7 ? (u32)((int32_t)host[rs1] >> rs2) : host[rs1] >> rs2;
            }
            else
                CHECK(0);
        }
        else if (op == 0x33)
        {
            unsigned funct7 = word >> 25;
            if (funct7 == 1 && (fn == 4 || fn == 6))
            {
                int32_t numerator = (int32_t)host[rs1];
                int32_t denominator = (int32_t)host[rs2];
                CHECK(denominator);
                if (numerator == INT32_MIN && denominator == -1)
                    host[rd] = fn == 4 ? (u32)INT32_MIN : 0;
                else
                    host[rd] = fn == 4 ? (u32)(numerator / denominator) :
                            (u32)(numerator % denominator);
            }
            else if (funct7 == 0 && fn == 6)
                host[rd] = host[rs1] | host[rs2];
            else if (funct7 == 0 && fn == 7)
                host[rd] = host[rs1] & host[rs2];
            else if (funct7 == 0 && fn == 0)
                host[rd] = host[rs1] + host[rs2];
            else if (funct7 == 0 && fn == 4)
                host[rd] = host[rs1] ^ host[rs2];
            else if (funct7 == 0x20 && fn == 0)
                host[rd] = host[rs1] - host[rs2];
            else if (funct7 == 0x20 && fn == 7)
                host[rd] = host[rs1] & ~host[rs2];
            else if (funct7 == 0x10 && (fn == 2 || fn == 4 || fn == 6))
                host[rd] = (host[rs1] << (fn / 2)) + host[rs2];
            else
                CHECK(0);
        }
        else if (op == 0x63)
        {
            CHECK(fn == 0 || fn == 1);
            if ((host[rs1] == host[rs2]) == (fn == 0))
            {
                uint32_t offset = ((word >> 31) << 12) |
                        (((word >> 7) & 1) << 11) |
                        (((word >> 25) & 63) << 5) |
                        (((word >> 8) & 15) << 1);
                int64_t target = (int64_t)pc + sign_extend(offset, 13);
                CHECK(target >= 0 && target <= (int64_t)bytes);
                next = (size_t)target;
            }
        }
        else
            CHECK(0);
        host[0] = 0;
        pc = next;
    }
    CHECK(pc == bytes);
}

static void test_decode_and_emit(void)
{
    reset_state();
    uint32_t words[64];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(gpsp_rv32_emit_arm_system(&code, 0xe10f2000, 0x08000000) ==
            GPSP_RV32_ARM_SYSTEM_EMITTED); /* MRS CPSR */
    CHECK((size_t)(code.ptr - code.start) == (GPSP_RV32_USE_ZBA ? 8 : 11) * 4);
    uint32_t host[32] = {0};
    host[RV_STATE] = 0x1000;
    host[RV_FLAG_N] = 1;
    host[RV_FLAG_Z] = 0;
    host[RV_FLAG_C] = 1;
    host[RV_FLAG_V] = 0;
    reg[REG_CPSR] = 0x6000001f; /* Deliberately stale NZCV in memory. */
    run_inline(words, (size_t)(code.ptr - code.start), host);
    CHECK(host[GUEST(2)] == 0xa000001f);
    for (unsigned nzcv = 0; nzcv < 16; nzcv++)
    {
        host[RV_FLAG_N] = nzcv >> 3;
        host[RV_FLAG_Z] = (nzcv >> 2) & 1;
        host[RV_FLAG_C] = (nzcv >> 1) & 1;
        host[RV_FLAG_V] = nzcv & 1;
        reg[REG_CPSR] = 0xf0abcdef;
        run_inline(words, (size_t)(code.ptr - code.start), host);
        CHECK(host[GUEST(2)] == ((nzcv << 28) | 0x0abcdef));
    }

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(gpsp_rv32_emit_arm_system(&code, 0xe128f000, 0x08000000) ==
            GPSP_RV32_ARM_SYSTEM_EMITTED); /* MSR CPSR_f,r0 */
    CHECK((size_t)(code.ptr - code.start) == (GPSP_RV32_USE_ZBS ? 4 : 7) * 4);
    memset(host, 0, sizeof(host));
    host[GUEST(0)] = 0x50000000;
    run_inline(words, (size_t)(code.ptr - code.start), host);
    CHECK(host[RV_FLAG_N] == 0 && host[RV_FLAG_Z] == 1 &&
            host[RV_FLAG_C] == 0 && host[RV_FLAG_V] == 1);
    host[GUEST(0)] = 0xafffffff;
    run_inline(words, (size_t)(code.ptr - code.start), host);
    CHECK(host[RV_FLAG_N] == 1 && host[RV_FLAG_Z] == 0 &&
            host[RV_FLAG_C] == 1 && host[RV_FLAG_V] == 0);

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(gpsp_rv32_emit_arm_system(&code, 0x010f2000, 0x08000000) ==
            GPSP_RV32_ARM_SYSTEM_EMITTED); /* MRS EQ, r2,CPSR */
    memset(host, 0, sizeof(host));
    host[RV_STATE] = 0x1000;
    host[GUEST(2)] = 0x12345678;
    run_inline(words, (size_t)(code.ptr - code.start), host);
    CHECK(host[GUEST(2)] == 0x12345678); /* Failed condition leaves Rd intact. */

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(gpsp_rv32_emit_arm_system(&code, 0xe3a00001, 0x08000000) ==
            GPSP_RV32_ARM_SYSTEM_UNSUPPORTED); /* MOV r0,#1 */
    CHECK(code.ptr == code.start);
    CHECK(gpsp_rv32_emit_arm_system(&code, 0xe10ff000, 0x08000000) ==
            GPSP_RV32_ARM_SYSTEM_UNSUPPORTED); /* MRS PC,CPSR */
    CHECK(code.ptr == code.start);
    CHECK(decode(0xe1b0f00e) == SYSTEM_EXCEPTION_RETURN); /* MOVS PC,LR */
    CHECK(decode(0xe3b0f001) == SYSTEM_EXCEPTION_RETURN); /* MOVS PC,#1 */
    CHECK(decode(0xe350f001) == SYSTEM_NONE); /* CMP cannot write PC. */
    CHECK(gpsp_rv32_emit_thumb_swi(&code, 0xde00, 0x08000000) ==
            GPSP_RV32_ARM_SYSTEM_UNSUPPORTED);
    CHECK(code.ptr == code.start);
    CHECK(gpsp_rv32_emit_arm_system(&code, 0xff000000, 0x08000000) ==
            GPSP_RV32_ARM_SYSTEM_EMITTED); /* NV SWI never enters the BIOS. */
    CHECK(gpsp_rv32_emit_arm_system(&code, 0xf1b0f00e, 0x08000000) ==
            GPSP_RV32_ARM_SYSTEM_EMITTED); /* NV MOVS PC,LR never returns. */
    CHECK(code.ptr == code.start);

    gpsp_rv32_code_init(&code, words, 4);
    CHECK(gpsp_rv32_emit_arm_system(&code, 0xe10f2000, 0x08000000) ==
            GPSP_RV32_ARM_SYSTEM_BUFFER_FULL);
    CHECK(code.ptr == code.start);
}

static void test_mrs_spsr_inline(void)
{
    static const struct { u32 mode, value; } cases[] = {
        {MODE_FIQ, 0xa1234591}, {MODE_IRQ, 0x57654392},
        {MODE_SUPERVISOR, 0x31234593}, {MODE_ABORT, 0xc7654397},
        {MODE_UNDEFINED, 0x9123459b},
    };
    uint32_t words[64], host[32], saved[6];
    gpsp_rv32_code code;
    reset_state();
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        REG_SPSR(cases[i].mode) = cases[i].value;
    memcpy(saved, spsr, sizeof(saved));
    reg[REG_CPSR] = 0xa000001f;
    CHECK(decode(0xe14f2000) == SYSTEM_MRS); /* MRS r2,SPSR */

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        reg[CPU_MODE] = cases[i].mode;
        gpsp_rv32_code_init(&code, words, sizeof(words));
        CHECK(emit_mrs(&code, 0xe14f2000, TEST_SPSR_BASE));
        memset(host, 0, sizeof(host));
        host[RV_STATE] = 0x1000;
        host[RV_FLAG_N] = host[RV_FLAG_C] = 1;
        host[GUEST(2)] = 0xdeadbeef;
        run_inline(words, (size_t)(code.ptr - code.start), host);
        CHECK(host[GUEST(2)] == cases[i].value);
        CHECK(host[RV_FLAG_N] == 1 && host[RV_FLAG_Z] == 0 &&
                host[RV_FLAG_C] == 1 && host[RV_FLAG_V] == 0);
        CHECK(reg[REG_CPSR] == 0xa000001f && reg[CPU_MODE] == cases[i].mode);
        CHECK(memcmp(saved, spsr, sizeof(saved)) == 0);
    }
}

static void test_msr_effects(void)
{
    reset_state();
    reg[REG_CPSR] = 0xa000001f;
    u32 cycles = 100;
    CHECK(msr_helper(reg, &cycles, 0x50000000, 2) & GPSP_RV32_HELPER_EXIT);
    CHECK(reg[REG_CPSR] == 0x5000001f);
    CHECK(reg[REG_PC] == 0x08000004 && cycles == 98);

    reset_state();
    REG_MODE(MODE_SUPERVISOR)[5] = 0x03007e00;
    reg[REG_SP] = 0x03007f00;
    cycles = 100;
    CHECK(msr_helper(reg, &cycles, 0x00000013, 1) & GPSP_RV32_HELPER_EXIT);
    CHECK(reg[CPU_MODE] == MODE_SUPERVISOR);
    CHECK(reg[REG_SP] == 0x03007e00);
    CHECK(reg[REG_PC] == 0x08000004 && cycles == 98);

    reset_state();
    reg[CPU_MODE] = MODE_USER;
    reg[REG_CPSR] = 0x10;
    cycles = 100;
    msr_helper(reg, &cycles, 0x00000013, 1);
    CHECK(reg[REG_CPSR] == 0x10 && reg[CPU_MODE] == MODE_USER);

    reset_state();
    reg[CPU_MODE] = MODE_SUPERVISOR;
    REG_SPSR(MODE_SUPERVISOR) = 0xa0000093;
    cycles = 100;
    msr_helper(reg, &cycles, 0x50000000, 6);
    CHECK(REG_SPSR(MODE_SUPERVISOR) == 0x50000093);
    CHECK(reg[REG_CPSR] == 0x1f);

    reset_state();
    raise_irq = true;
    cycles = 100;
    msr_helper(reg, &cycles, 0x0000001f, 1);
    CHECK(reg[REG_PC] == 0x18 && reg[CPU_MODE] == MODE_IRQ);
    CHECK(cycles == 100); /* IRQ enters before the sequential fetch. */
}

static void test_msr_spsr_inline(void)
{
    uint32_t words[64], host[32];
    gpsp_rv32_code code;
    CHECK(decode(0xe161f000) == SYSTEM_MSR_REG);
    CHECK(decode(0xe168f000) == SYSTEM_MSR_REG);
    CHECK(decode(0xe169f000) == SYSTEM_MSR_REG);
    CHECK(decode(0xe368f480) == SYSTEM_MSR_IMM);
    reset_state();
    reg[CPU_MODE] = MODE_SUPERVISOR;
    reg[REG_CPSR] = 0x30000093;
    REG_SPSR(MODE_SUPERVISOR) = 0xa1234593;

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_msr_spsr(&code, 0xe161f000, SYSTEM_MSR_REG, 1,
            TEST_SPSR_BASE));
    memset(host, 0, sizeof(host));
    host[RV_STATE] = 0x1000;
    host[GUEST(0)] = 0x0000000f;
    host[RV_FLAG_N] = 1;
    run_inline(words, (size_t)(code.ptr - code.start), host);
    CHECK(REG_SPSR(MODE_SUPERVISOR) == 0xa123451f);
    CHECK(reg[REG_CPSR] == 0x30000093 && host[RV_FLAG_N] == 1);

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_msr_spsr(&code, 0xe168f000, SYSTEM_MSR_REG, 2,
            TEST_SPSR_BASE));
    memset(host, 0, sizeof(host));
    host[RV_STATE] = 0x1000;
    host[GUEST(0)] = 0x50000000;
    run_inline(words, (size_t)(code.ptr - code.start), host);
    CHECK(REG_SPSR(MODE_SUPERVISOR) == 0x5123451f);

    reg[CPU_MODE] = MODE_IRQ;
    REG_SPSR(MODE_IRQ) = 0xa7654392;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_msr_spsr(&code, 0xe169f000, SYSTEM_MSR_REG, 3,
            TEST_SPSR_BASE));
    memset(host, 0, sizeof(host));
    host[RV_STATE] = 0x1000;
    host[GUEST(0)] = 0x3fffff0f;
    run_inline(words, (size_t)(code.ptr - code.start), host);
    CHECK(REG_SPSR(MODE_IRQ) == 0x3765431f);
    CHECK(REG_SPSR(MODE_SUPERVISOR) == 0x5123451f);

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_msr_spsr(&code, 0xe368f480, SYSTEM_MSR_IMM, 2,
            TEST_SPSR_BASE));
    memset(host, 0, sizeof(host));
    host[RV_STATE] = 0x1000;
    run_inline(words, (size_t)(code.ptr - code.start), host);
    CHECK(REG_SPSR(MODE_IRQ) == 0x8765431f);

    gpsp_rv32_arm_condition_skip skip = {{NULL, NULL}, 0};
    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(gpsp_rv32_emit_arm_condition_skip(&code, 0, &skip));
    CHECK(emit_msr_spsr(&code, 0x0168f000, SYSTEM_MSR_REG, 2,
            TEST_SPSR_BASE));
    CHECK(gpsp_rv32_patch_arm_condition_skip(&code, &skip, code.ptr));
    memset(host, 0, sizeof(host));
    host[RV_STATE] = 0x1000;
    host[GUEST(0)] = 0x20000000;
    run_inline(words, (size_t)(code.ptr - code.start), host);
    CHECK(REG_SPSR(MODE_IRQ) == 0x8765431f);
    host[RV_FLAG_Z] = 1;
    run_inline(words, (size_t)(code.ptr - code.start), host);
    CHECK(REG_SPSR(MODE_IRQ) == 0x2765431f);

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_msr_spsr(&code, 0xe160f000, SYSTEM_MSR_REG, 0,
            TEST_SPSR_BASE));
    CHECK(code.ptr == code.start);
}

static void test_div_body(void)
{
    uint32_t words[16];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_div_body(&code, false));
    CHECK((size_t)(code.ptr - code.start) == 11 * 4);

    uint32_t host[32] = {0};
    host[RV_STATE] = 0x1000;
    host[GUEST(0)] = (u32)-1234;
    host[GUEST(1)] = 10;
    host[RV_BUDGET] = 100;
    run_inline(words, (size_t)(code.ptr - code.start), host);
    CHECK(host[GUEST(0)] == (u32)-123 && host[GUEST(1)] == (u32)-4);
    CHECK(host[GUEST(3)] == 123 && host[RV_BUDGET] == 36);
    CHECK(reg[REG_BUS_VALUE] == 0xe3a02004u);

    memset(host, 0, sizeof(host));
    host[RV_STATE] = 0x1000;
    host[GUEST(0)] = (u32)INT32_MIN;
    host[GUEST(1)] = (u32)-1;
    host[RV_BUDGET] = 100;
    run_inline(words, (size_t)(code.ptr - code.start), host);
    CHECK(host[GUEST(0)] == (u32)INT32_MIN && host[GUEST(1)] == 0);
    CHECK(host[GUEST(3)] == 0x80000000u && host[RV_BUDGET] == 36);
    CHECK(reg[REG_BUS_VALUE] == 0xe3a02004u);

    gpsp_rv32_code_init(&code, words, sizeof(words));
    CHECK(emit_div_body(&code, true));
    memset(host, 0, sizeof(host));
    host[RV_STATE] = 0x1000;
    host[GUEST(0)] = 10; /* DivArm takes denominator in r0. */
    host[GUEST(1)] = (u32)-1234;
    host[RV_BUDGET] = 100;
    run_inline(words, (size_t)(code.ptr - code.start), host);
    CHECK(host[GUEST(0)] == (u32)-123 && host[GUEST(1)] == (u32)-4);
    CHECK(host[GUEST(3)] == 123 && host[RV_BUDGET] == 36);
    CHECK(reg[REG_BUS_VALUE] == 0xe3a02004u);
}

static void test_swi_effect(void)
{
    reset_state();
    reg[REG_CPSR] = 0xa000001f;
    reg[REG_SP] = 0x03007f00;
    REG_MODE(MODE_SUPERVISOR)[5] = 0x03007e00;
    ws_cyc_seq[0][1] = 1;
    u32 cycles = 100;
    CHECK(swi_helper(reg, &cycles, 0, 0) & GPSP_RV32_HELPER_EXIT);
    CHECK(reg[REG_PC] == 8 && reg[CPU_MODE] == MODE_SUPERVISOR);
    CHECK(reg[REG_CPSR] == 0xa0000093);
    CHECK(REG_SPSR(MODE_SUPERVISOR) == 0xa000001f);
    CHECK(REG_MODE(MODE_SUPERVISOR)[6] == 0x08000004);
    CHECK(reg[REG_SP] == 0x03007e00);
    CHECK(reg[REG_BUS_VALUE] == 0xe3a02004 && cycles == 99);

    reset_state();
    reg[REG_CPSR] = 0x6000003f;
    cycles = 100;
    CHECK(thumb_swi_helper(reg, &cycles, 0, 0) & GPSP_RV32_HELPER_EXIT);
    CHECK(reg[REG_PC] == 8 && reg[CPU_MODE] == MODE_SUPERVISOR);
    CHECK(reg[REG_CPSR] == 0x60000093);
    CHECK(REG_SPSR(MODE_SUPERVISOR) == 0x6000003f);
    CHECK(REG_MODE(MODE_SUPERVISOR)[6] == 0x08000002);
    CHECK(reg[REG_BUS_VALUE] == 0xe3a02004 && cycles == 100);
}

static void test_exception_return_effect(void)
{
    reset_state();
    reg[CPU_MODE] = MODE_IRQ;
    reg[REG_CPSR] = 0x92;
    reg[REG_SP] = 0x03007f00;
    REG_MODE(MODE_SYSTEM)[5] = 0x03007e00;
    REG_SPSR(MODE_IRQ) = 0x3f; /* Return to system Thumb mode. */
    u32 cycles = 100;
    CHECK(exception_return_helper(reg, &cycles, 0x08000103, 0) &
            GPSP_RV32_HELPER_EXIT);
    CHECK(reg[REG_PC] == 0x08000102);
    CHECK(reg[REG_CPSR] == 0x3f && reg[CPU_MODE] == MODE_SYSTEM);
    CHECK(reg[REG_SP] == 0x03007e00);
    CHECK(REG_MODE(MODE_IRQ)[5] == 0x03007f00);
    CHECK(cycles == 100); /* Thumb loop handles its own next fetch. */

    reset_state();
    reg[CPU_MODE] = MODE_IRQ;
    reg[REG_CPSR] = 0x92;
    REG_SPSR(MODE_IRQ) = 0x1f;
    cycles = 100;
    CHECK(exception_return_helper(reg, &cycles, 0x08000143, 0) &
            GPSP_RV32_HELPER_EXIT);
    CHECK(reg[REG_PC] == 0x08000140);
    CHECK(reg[REG_CPSR] == 0x1f && reg[CPU_MODE] == MODE_SYSTEM);
    CHECK(cycles == 98);

    reset_state();
    reg[CPU_MODE] = MODE_IRQ;
    reg[REG_CPSR] = 0x92;
    REG_SPSR(MODE_IRQ) = 0x1f;
    raise_irq = true;
    cycles = 100;
    CHECK(exception_return_helper(reg, &cycles, 0x08000144, 0) &
            GPSP_RV32_HELPER_EXIT);
    CHECK(reg[REG_PC] == 0x18 && reg[CPU_MODE] == MODE_IRQ);
    CHECK(REG_MODE(MODE_IRQ)[6] == 0x08000148);
    CHECK(REG_SPSR(MODE_IRQ) == 0x1f && cycles == 100);
}

static void test_swap_effects(void)
{
    reset_state();
    ws_cyc_nseq[2][1] = 3;
    memory_value = 0x33aa55cc;
    write_alert = CPU_ALERT_SMC;
    u32 cycles = 100;
    uint64_t result = swap_word_helper(reg, &cycles, 0x02000001, 0x7f00ff11);
    CHECK((u32)result == 0xcc33aa55);
    CHECK(result & GPSP_RV32_HELPER_EXIT);
    CHECK(read_count == 1 && read_address == 0x02000001);
    CHECK(write_count == 1 && write_address == 0x02000000);
    CHECK(write_value == 0x7f00ff11);
    CHECK(reg[REG_PC] == 0x08000004 && cycles == 91);
    CHECK(gpsp_rv32_arm_system_take_alert() == CPU_ALERT_SMC);
    CHECK(gpsp_rv32_arm_system_take_alert() == 0);

    reset_state();
    ws_cyc_nseq[3][0] = 1;
    memory_value = 0x000000d3;
    cycles = 100;
    result = swap_byte_helper(reg, &cycles, 0x03000003, 0xabcdef12);
    CHECK((u32)result == 0xd3);
    CHECK(read_address == 0x03000003 && write_address == 0x03000003);
    CHECK(write_value == 0x12 && cycles == 95);
}

/* Moving successful output by two bytes preserves its contents. */
static void check_moved(const uint32_t *words, const gpsp_rv32_code *code,
        const gpsp_rv32_code *moved)
{
    size_t size = (size_t)(code->ptr - code->start);
    CHECK(size && (size_t)(moved->ptr - moved->start) == size &&
            !memcmp(moved->start, words, size));
}

static void test_two_byte_position(void)
{
    static const uint32_t opcodes[] = {0xe10f2000, 0xe128f000, 0x010f2000};
    uint32_t words[192], shifted[193];
    gpsp_rv32_code code, moved;
    for (unsigned i = 0; i < 3; i++)
    {
        gpsp_rv32_code_init(&code, words, sizeof(words));
        gpsp_rv32_code_child(&moved, &code, (uint8_t *)shifted + 2,
                sizeof(words));
        CHECK(gpsp_rv32_emit_arm_system(&code, opcodes[i], 0x08000000) ==
                GPSP_RV32_ARM_SYSTEM_EMITTED);
        CHECK(gpsp_rv32_emit_arm_system(&moved, opcodes[i], 0x08000000) ==
                GPSP_RV32_ARM_SYSTEM_EMITTED);
        check_moved(words, &code, &moved);
    }

    gpsp_rv32_code_init(&code, words, sizeof(words));
    gpsp_rv32_code_child(&moved, &code, (uint8_t *)shifted + 2,
            sizeof(words));
    CHECK(emit_mrs(&code, 0xe14f2000, TEST_SPSR_BASE));
    CHECK(emit_mrs(&moved, 0xe14f2000, TEST_SPSR_BASE));
    check_moved(words, &code, &moved);

    gpsp_rv32_code_init(&code, words, sizeof(words));
    gpsp_rv32_code_child(&moved, &code, (uint8_t *)shifted + 2,
            sizeof(words));
    CHECK(emit_msr_spsr(&code, 0xe169f000, SYSTEM_MSR_REG, 1,
            TEST_SPSR_BASE));
    CHECK(emit_msr_spsr(&moved, 0xe169f000, SYSTEM_MSR_REG, 1,
            TEST_SPSR_BASE));
    check_moved(words, &code, &moved);

    for (int swapped = 0; swapped < 2; swapped++)
    {
        gpsp_rv32_code_init(&code, words, sizeof(words));
        gpsp_rv32_code_child(&moved, &code, (uint8_t *)shifted + 2,
                sizeof(words));
        CHECK(emit_div_body(&code, swapped));
        CHECK(emit_div_body(&moved, swapped));
        check_moved(words, &code, &moved);
    }
}

int main(void)
{
    /* Exact sizes below assume 32-bit instructions. */
    gpsp_rv32_test_compress = false;
    test_decode_and_emit();
    test_mrs_spsr_inline();
    test_msr_effects();
    test_msr_spsr_inline();
    test_div_body();
    test_swi_effect();
    test_exception_return_effect();
    test_swap_effects();
    test_two_byte_position();
    puts("arm system ok");
    return 0;
}
