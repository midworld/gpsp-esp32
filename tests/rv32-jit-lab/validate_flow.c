/* Thumb flow emitter validation on a small RV32I executor.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rv32_thumb_flow_emit.h"
#include "rv32_block_link.h"
#include "rv32_register_map.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    START_PC = 0x08000100,
    CODE_BASE = 0x30000000,
    RETURN_PC = 0x40000000,
    STUB_PC = 0x50002000,
    SEQ_ADDRESS = 0x20001230,
    NSEQ_ADDRESS = 0x20001330,
    MAX_STEPS = 512,
};

typedef enum
{
    EXIT_COMPLETE,
    EXIT_COLD_STEP,
    EXIT_NATIVE_RESUME,
} exit_kind;

typedef struct
{
    uint32_t guest[15];
    uint32_t pc;
    int32_t budget;
    uint8_t n, z, c, v;
} thumb_state;

typedef struct
{
    uint32_t regs[32];
    uint32_t native_pc;
    exit_kind exit;
} rv32_state;

typedef struct
{
    const char *name;
    const uint16_t *opcodes;
    unsigned count;
    unsigned entry;
    unsigned seq, nseq;
    int32_t budget;
    uint32_t r0;
    unsigned flags;
    bool chained;
} test_case;

static void fail(const char *message, const test_case *test)
{
    fprintf(stderr, "%s: %s entry=%u wait=%u/%u budget=%" PRId32
            " r0=%08" PRIx32 " flags=%x\n", message, test->name, test->entry,
            test->seq, test->nseq, test->budget, test->r0, test->flags);
    exit(1);
}

static int32_t sign_extend(uint32_t bits, unsigned width)
{
    uint32_t sign = 1u << (width - 1);
    return (int32_t)((bits ^ sign) - sign);
}

static void init_state(thumb_state *expected, rv32_state *rv,
        const test_case *test)
{
    memset(expected, 0, sizeof(*expected));
    memset(rv, 0, sizeof(*rv));
    for (unsigned i = 0; i < 15; i++)
    {
        expected->guest[i] = 0x19324567u ^ (0x01010101u * i);
        rv->regs[guest_reg_map[i]] = expected->guest[i];
    }
    expected->guest[0] = test->r0;
    rv->regs[guest_reg_map[0]] = test->r0;
    expected->pc = START_PC + test->entry * 2;
    expected->budget = test->budget;
    expected->n = (test->flags >> 3) & 1;
    expected->z = (test->flags >> 2) & 1;
    expected->c = (test->flags >> 1) & 1;
    expected->v = test->flags & 1;
    rv->regs[RA] = RETURN_PC;
    rv->regs[RV_BUDGET] = (uint32_t)expected->budget;
    rv->regs[RV_PC] = expected->pc;
    rv->regs[RV_FLAG_N] = expected->n;
    rv->regs[RV_FLAG_Z] = expected->z;
    rv->regs[RV_FLAG_C] = expected->c;
    rv->regs[RV_FLAG_V] = expected->v;
}

static void thumb_step(thumb_state *state, const test_case *test)
{
    unsigned index = (state->pc - START_PC) / 2;
    uint16_t opcode = test->opcodes[index];
    if ((opcode & 0xffc0) == 0x4200)
    {
        unsigned rd = opcode & 7, rs = (opcode >> 3) & 7;
        uint32_t value = state->guest[rd] & state->guest[rs];
        state->n = value >> 31;
        state->z = value == 0;
        state->pc += 2;
        state->budget -= test->seq;
    }
    else if ((opcode & 0xff00) == 0xd000)
    {
        state->pc = state->z ? state->pc + 4 + (int8_t)(opcode & 0xff) * 2 :
                state->pc + 2;
        state->budget -= test->seq + test->nseq;
    }
    else if ((opcode & 0xf800) == 0x2000)
    {
        unsigned rd = (opcode >> 8) & 7;
        state->guest[rd] = opcode & 0xff;
        state->n = 0;
        state->z = state->guest[rd] == 0;
        state->pc += 2;
        state->budget -= test->seq;
    }
    else
        fail("unsupported reference opcode", test);
}

static exit_kind reference_run(thumb_state *state, thumb_state *before_cold,
        const test_case *test)
{
    while (state->pc < START_PC + test->count * 2)
    {
        if (state->budget <= 0)
            return EXIT_NATIVE_RESUME;
        unsigned index = (state->pc - START_PC) / 2;
        unsigned end = test->count;
        if (test->count == 4)
            end = index <= 1 ? 2 : index + 1;
        else if (test->chained && index >= 30)
            end = index + 1;
        unsigned cost = (end - index) * test->seq;
        if (test->count == 4 && end == 2)
            cost += test->nseq;
        unsigned worst = (end - index + (test->count == 4 && end == 2)) * 9;
        if (worst > 64 && state->budget < (int32_t)cost)
        {
            *before_cold = *state;
            thumb_step(state, test);
            return EXIT_COLD_STEP;
        }
        while (state->pc < START_PC + end * 2)
            thumb_step(state, test);
    }
    return EXIT_COMPLETE;
}

static uint32_t read_word(const uint8_t *code, uint32_t offset)
{
    uint32_t word;
    memcpy(&word, code + offset, sizeof(word));
    return word;
}

static void rv32_run(rv32_state *rv, const uint8_t *code, size_t code_bytes,
        const test_case *test)
{
    for (unsigned steps = 0; steps < MAX_STEPS; steps++)
    {
        uint32_t offset = rv->native_pc - CODE_BASE;
        if ((offset & 3) || (size_t)offset + 4 > code_bytes)
            fail("native PC outside generated code", test);
        uint32_t insn = read_word(code, offset);
        uint32_t opcode = insn & 0x7f, rd = (insn >> 7) & 31;
        uint32_t funct3 = (insn >> 12) & 7;
        uint32_t rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31;
        uint32_t next_pc = rv->native_pc + 4;
        int32_t immediate = sign_extend(insn >> 20, 12);

        switch (opcode)
        {
            case 0x03:
            {
                if (funct3 != 4)
                    fail("unexpected load", test);
                uint32_t address = rv->regs[rs1] + immediate;
                if (address == SEQ_ADDRESS)
                    rv->regs[rd] = test->seq;
                else if (address == NSEQ_ADDRESS)
                    rv->regs[rd] = test->nseq;
                else
                    fail("unexpected wait-table address", test);
                break;
            }
            case 0x13:
                if (funct3 == 0)
                    rv->regs[rd] = rv->regs[rs1] + immediate;
                else if (funct3 == 1 && (insn >> 25) == 0)
                    rv->regs[rd] = rv->regs[rs1] << (immediate & 31);
                else if (funct3 == 5 && (insn >> 25) == 0)
                    rv->regs[rd] = rv->regs[rs1] >> (immediate & 31);
                else if (funct3 == 3)
                    rv->regs[rd] = rv->regs[rs1] < (uint32_t)immediate;
                else
                    fail("unexpected immediate operation", test);
                break;
            case 0x33:
                if (funct3 == 0 && (insn >> 25) == 0)
                    rv->regs[rd] = rv->regs[rs1] + rv->regs[rs2];
                else if (funct3 == 0 && (insn >> 25) == 0x20)
                    rv->regs[rd] = rv->regs[rs1] - rv->regs[rs2];
                else if (funct3 == 7 && (insn >> 25) == 0)
                    rv->regs[rd] = rv->regs[rs1] & rv->regs[rs2];
                else
                    fail("unexpected register operation", test);
                break;
            case 0x37:
                rv->regs[rd] = insn & 0xfffff000u;
                break;
            case 0x63:
            {
                uint32_t bits = ((insn >> 31) << 12) |
                        (((insn >> 7) & 1) << 11) |
                        (((insn >> 25) & 0x3f) << 5) |
                        (((insn >> 8) & 0xf) << 1);
                bool taken = false;
                if (funct3 == 0)
                    taken = rv->regs[rs1] == rv->regs[rs2];
                else if (funct3 == 1)
                    taken = rv->regs[rs1] != rv->regs[rs2];
                else if (funct3 == 4)
                    taken = (int32_t)rv->regs[rs1] < (int32_t)rv->regs[rs2];
                else if (funct3 == 5)
                    taken = (int32_t)rv->regs[rs1] >= (int32_t)rv->regs[rs2];
                else
                    fail("unexpected branch", test);
                if (taken)
                    next_pc = rv->native_pc + sign_extend(bits, 13);
                break;
            }
            case 0x6f:
            {
                uint32_t bits = ((insn >> 31) << 20) |
                        (((insn >> 12) & 0xff) << 12) |
                        (((insn >> 20) & 1) << 11) |
                        (((insn >> 21) & 0x3ff) << 1);
                if (rd)
                    rv->regs[rd] = next_pc;
                next_pc = rv->native_pc + sign_extend(bits, 21);
                break;
            }
            case 0x67:
            {
                if (funct3 != 0)
                    fail("unexpected JALR", test);
                uint32_t target = (rv->regs[rs1] + immediate) & ~1u;
                if (rd)
                    rv->regs[rd] = next_pc;
                if (target == RETURN_PC && rd == 0)
                    rv->exit = EXIT_COMPLETE;
                else if (target == RETURN_PC + 4 && rd == A1)
                    rv->exit = EXIT_NATIVE_RESUME;
                else if (target == STUB_PC && rd == 0)
                    rv->exit = EXIT_COLD_STEP;
                else if (target >= CODE_BASE &&
                        (size_t)(target - CODE_BASE) < code_bytes && rd == 0)
                {
                    next_pc = target;
                    break;
                }
                else
                    fail("unexpected native exit target", test);
                return;
            }
            default:
                fail("unexpected RV32 opcode", test);
        }
        rv->regs[0] = 0;
        rv->native_pc = next_pc;
    }
    fail("RV32 step limit exceeded", test);
}

static void cold_step(rv32_state *rv, const test_case *test)
{
    thumb_state state;
    memset(&state, 0, sizeof(state));
    for (unsigned i = 0; i < 15; i++)
        state.guest[i] = rv->regs[guest_reg_map[i]];
    state.pc = rv->regs[RV_PC];
    state.budget = (int32_t)rv->regs[RV_BUDGET];
    state.n = rv->regs[RV_FLAG_N];
    state.z = rv->regs[RV_FLAG_Z];
    state.c = rv->regs[RV_FLAG_C];
    state.v = rv->regs[RV_FLAG_V];
    thumb_step(&state, test);
    for (unsigned i = 0; i < 15; i++)
        rv->regs[guest_reg_map[i]] = state.guest[i];
    rv->regs[RV_PC] = state.pc;
    rv->regs[RV_BUDGET] = (uint32_t)state.budget;
    rv->regs[RV_FLAG_N] = state.n;
    rv->regs[RV_FLAG_Z] = state.z;
    rv->regs[RV_FLAG_C] = state.c;
    rv->regs[RV_FLAG_V] = state.v;
}

static void compare_state(const thumb_state *expected, const rv32_state *rv,
        const test_case *test)
{
    for (unsigned i = 0; i < 15; i++)
        if (expected->guest[i] != rv->regs[guest_reg_map[i]])
        {
            fprintf(stderr, "r%u expected=%08" PRIx32 " actual=%08" PRIx32 "\n",
                    i, expected->guest[i], rv->regs[guest_reg_map[i]]);
            fail("guest register mismatch", test);
        }
    if (expected->pc != rv->regs[RV_PC] ||
            (uint32_t)expected->budget != rv->regs[RV_BUDGET] ||
            expected->n != rv->regs[RV_FLAG_N] ||
            expected->z != rv->regs[RV_FLAG_Z] ||
            expected->c != rv->regs[RV_FLAG_C] ||
            expected->v != rv->regs[RV_FLAG_V])
    {
        fprintf(stderr, "expected pc=%08" PRIx32 " budget=%" PRId32
                " NZCV=%u%u%u%u; actual pc=%08" PRIx32 " budget=%" PRId32
                " NZCV=%u%u%u%u\n", expected->pc, expected->budget,
                expected->n, expected->z, expected->c, expected->v,
                rv->regs[RV_PC], (int32_t)rv->regs[RV_BUDGET],
                rv->regs[RV_FLAG_N], rv->regs[RV_FLAG_Z],
                rv->regs[RV_FLAG_C], rv->regs[RV_FLAG_V]);
        fail("guest state mismatch", test);
    }
}

static unsigned validate_case(const uint8_t *code, size_t bytes,
        uint32_t entry_offset, const test_case *test)
{
    thumb_state expected;
    thumb_state before_cold;
    rv32_state rv;
    init_state(&expected, &rv, test);
    exit_kind exit = reference_run(&expected, &before_cold, test);
    rv.native_pc = CODE_BASE + entry_offset;
    rv32_run(&rv, code, bytes, test);
    if (rv.exit != exit)
        fail("exit kind mismatch", test);
    if (rv.exit == EXIT_COLD_STEP)
    {
        compare_state(&before_cold, &rv, test);
        cold_step(&rv, test);
    }
    compare_state(&expected, &rv, test);
    return 1;
}

static unsigned validate_program(const char *name, const uint16_t *opcodes,
        unsigned count, bool side)
{
    uint32_t words[1024];
    gpsp_rv32_code main;
    gpsp_rv32_code_init(&main, words, sizeof(words));
    gpsp_rv32_thumb_ir ir;
    if (!gpsp_rv32_thumb_ir_decode(START_PC, opcodes, count, &ir))
    {
        fputs("flow IR decode failed\n", stderr);
        exit(1);
    }
    gpsp_rv32_thumb_flow_environment env = {
        .sequential_wait_address = SEQ_ADDRESS,
        .nonsequential_wait_address = NSEQ_ADDRESS,
        .interpreter_stub_address = STUB_PC,
        .idle_loop_target_pc = UINT32_MAX,
    };
    gpsp_rv32_thumb_flow_layout layout;
    if (!gpsp_rv32_thumb_flow_emit(&ir, &env, &main, &layout))
    {
        fputs("flow main emission failed\n", stderr);
        exit(1);
    }
    size_t side_offset = layout.code_bytes;
    if (side)
    {
        uint64_t published = UINT64_C(1) << count;
        for (unsigned i = 0; i < count; i++)
            if (layout.entry_offset[i] != UINT16_MAX)
                published |= UINT64_C(1) << i;
        gpsp_rv32_code detached;
        gpsp_rv32_code_init(&detached, main.ptr,
                (size_t)(main.end - main.ptr));
        gpsp_rv32_thumb_flow_side_layout side_layout;
        if (!gpsp_rv32_thumb_flow_emit_side(&ir, &env, 1, 2, published, &layout,
                &detached, &side_layout))
        {
            fputs("flow side emission failed\n", stderr);
            exit(1);
        }
        main.ptr = detached.ptr;
        for (unsigned i = 0; i < side_layout.link_count; i++)
        {
            unsigned target = side_layout.link_target[i];
            if (target >= count || layout.entry_offset[target] == UINT16_MAX ||
                !gpsp_rv32_patch_link_slot(&main,
                        (uint8_t *)words + side_offset + side_layout.link_offset[i],
                        CODE_BASE + layout.entry_offset[target]))
            {
                fputs("flow side link failed\n", stderr);
                exit(1);
            }
        }
    }
    size_t bytes = (size_t)(main.ptr - (uint8_t *)words);
    unsigned cases = 0;
    unsigned entries = side ? 2 : 1;
    for (unsigned e = 0; e < entries; e++)
    {
        unsigned entry = e ? 1 : 0;
        unsigned first_cost = (side && entry ? 1 : count == 4 ? 2 : count);
        for (unsigned seq = 1; seq <= 9; seq += seq == 1 ? 2 : 6)
            for (unsigned nseq = 1; nseq <= 9; nseq += nseq == 1 ? 2 : 6)
            {
                unsigned cost = first_cost * seq + (count == 4 ? nseq : 0);
                int budgets[] = {-1, 0, 1, (int)cost - 1,
                        (int)cost, (int)cost + 1, (int)cost + 2,
                        (int)cost + 2 * (int)seq + 1, 512};
                for (unsigned b = 0; b < sizeof(budgets) / sizeof(budgets[0]); b++)
                    for (unsigned flags = 0; flags < 16; flags++)
                        for (unsigned value = 0; value < (count == 4 ? 3u : 1u); value++)
                        {
                            const uint32_t r0[] = {0, 1, 0x80000000u};
                            test_case test = {name, opcodes, count, entry, seq, nseq,
                                    budgets[b], r0[value], flags, false};
                            cases += validate_case((const uint8_t *)words, bytes,
                                    entry ? (uint32_t)side_offset : layout.entry_offset[0],
                                    &test);
                        }
            }
    }
    printf("%s code_bytes=%u side_bytes=%zu cases=%u\n", name,
            layout.code_bytes, bytes - layout.code_bytes, cases);
    return cases;
}

static unsigned validate_chained_side(const uint16_t *opcodes)
{
    uint32_t words[1024];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    gpsp_rv32_thumb_ir ir;
    gpsp_rv32_thumb_flow_environment env = {
        .sequential_wait_address = SEQ_ADDRESS,
        .nonsequential_wait_address = NSEQ_ADDRESS,
        .interpreter_stub_address = STUB_PC,
        .idle_loop_target_pc = UINT32_MAX,
    };
    gpsp_rv32_thumb_flow_layout main_layout;
    if (!gpsp_rv32_thumb_ir_decode(START_PC, opcodes, 32, &ir) ||
        !gpsp_rv32_thumb_flow_emit(&ir, &env, &code, &main_layout))
    {
        fputs("chained main emission failed\n", stderr);
        exit(1);
    }

    unsigned offsets[2];
    gpsp_rv32_thumb_flow_side_layout sides[2];
    uint64_t published = (UINT64_C(1) << 0) | (UINT64_C(1) << 32);
    for (unsigned i = 0; i < 2; i++)
    {
        unsigned entry = 31 - i;
        offsets[i] = (unsigned)(code.ptr - (uint8_t *)words);
        gpsp_rv32_code detached;
        gpsp_rv32_code_init(&detached, code.ptr,
                (size_t)(code.end - code.ptr));
        if (!gpsp_rv32_thumb_flow_emit_side(&ir, &env, entry, entry + 1,
                published,
                &main_layout, &detached, &sides[i]) ||
                sides[i].link_count != 1 ||
                sides[i].link_target[0] != entry + 1)
        {
            fputs("chained side emission failed\n", stderr);
            exit(1);
        }
        code.ptr = detached.ptr;
        published |= UINT64_C(1) << entry;
    }
    unsigned targets[2] = {main_layout.exit_link_offset, offsets[0]};
    for (unsigned i = 0; i < 2; i++)
        if (!gpsp_rv32_patch_link_slot(&code,
                (uint8_t *)words + offsets[i] + sides[i].link_offset[0],
                CODE_BASE + targets[i]))
        {
            fputs("chained side link failed\n", stderr);
            exit(1);
        }

    unsigned cases = 0;
    for (unsigned seq = 1; seq <= 9; seq += 8)
        for (unsigned b = 0; b < 2; b++)
            for (unsigned flags = 0; flags <= 15; flags += 15)
            {
                test_case test = {"chained_side", opcodes, 32, 30, seq, seq,
                        b ? (int32_t)(seq * 2 + 1) : 1, 0, flags, true};
                cases += validate_case((const uint8_t *)words,
                        (size_t)(code.ptr - (uint8_t *)words), offsets[1], &test);
            }
    printf("chained_side code_bytes=%zu cases=%u\n",
            (size_t)(code.ptr - (uint8_t *)words) - main_layout.code_bytes,
            cases);
    return cases;
}

static unsigned validate_edge_program(const char *name,
        const uint16_t *opcodes, unsigned count)
{
    uint32_t words[512];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    gpsp_rv32_thumb_ir ir;
    gpsp_rv32_thumb_flow_environment env = {
        .sequential_wait_address = SEQ_ADDRESS,
        .nonsequential_wait_address = NSEQ_ADDRESS,
        .interpreter_stub_address = STUB_PC,
        .idle_loop_target_pc = UINT32_MAX,
    };
    gpsp_rv32_thumb_flow_layout layout;
    if (!gpsp_rv32_thumb_ir_decode(START_PC, opcodes, count, &ir) ||
        !gpsp_rv32_thumb_flow_emit(&ir, &env, &code, &layout))
    {
        fputs("edge program emission failed\n", stderr);
        exit(1);
    }

    unsigned cases = 0;
    const uint32_t r0[] = {0, 1, 2, UINT32_MAX};
    for (unsigned seq = 1; seq <= 9; seq += 8)
        for (unsigned value = 0; value < sizeof(r0) / sizeof(r0[0]); value++)
            for (unsigned flags = 0; flags <= 15; flags += 15)
            {
                int budgets[] = {0, 1, (int)(2 * seq + seq), 256};
                for (unsigned b = 0; b < sizeof(budgets) / sizeof(budgets[0]); b++)
                {
                    if (count > 4 && b != 0 && b != 3)
                        continue;
                    test_case test = {name, opcodes, count, 0, seq, seq,
                            budgets[b], r0[value], flags, false};
                    cases += validate_case((const uint8_t *)words,
                            (size_t)(code.ptr - (uint8_t *)words), 0, &test);
                }
            }
    printf("%s code_bytes=%u cases=%u\n", name, layout.code_bytes,
            cases);
    return cases;
}

static void validate_region_boundary(const uint16_t *opcodes)
{
    gpsp_rv32_thumb_ir ir;
    uint32_t words[128];
    gpsp_rv32_code code;
    gpsp_rv32_code_init(&code, words, sizeof(words));
    gpsp_rv32_thumb_flow_environment env = {
        .sequential_wait_address = SEQ_ADDRESS,
        .nonsequential_wait_address = NSEQ_ADDRESS,
        .interpreter_stub_address = STUB_PC,
        .idle_loop_target_pc = UINT32_MAX,
    };
    gpsp_rv32_thumb_flow_layout layout;
    if (!gpsp_rv32_thumb_ir_decode(START_PC, opcodes, 4, &ir))
    {
        fputs("boundary IR decode failed\n", stderr);
        exit(1);
    }
    ir.pc = 0x0dfffffc;
    if (gpsp_rv32_thumb_flow_emit(&ir, &env, &code, &layout) ||
        code.ptr != code.start)
    {
        fputs("cross-region flow was accepted\n", stderr);
        exit(1);
    }
}

int main(void)
{
    const uint16_t flow[4] = {0x4200, 0xd000, 0x2102, 0x2203};
    uint16_t cap[32];
    for (unsigned i = 0; i < 32; i++)
        cap[i] = 0x2100 | ((i + 1) & 0xff);
    cap[31] = 0x2100;
    unsigned cases = validate_program("branch", flow, 4, true);
    cases += validate_program("scan_cap", cap, 32, false);
    cases += validate_chained_side(cap);
    const uint16_t end_branch[4] = {0x4208, 0xd001, 0x2201, 0x2300};
    const uint16_t two_taken[8] = {0x4208, 0xd001, 0x2200, 0xd002,
            0x2305, 0x2400, 0x2506, 0x2603};
    const uint16_t two_untaken[8] = {0x4208, 0xd001, 0x2201, 0xd002,
            0x2305, 0x2400, 0x2506, 0x2603};
    cases += validate_edge_program("branch_to_end", end_branch, 4);
    cases += validate_edge_program("two_beq_taken", two_taken, 8);
    cases += validate_edge_program("two_beq_untaken", two_untaken, 8);
    validate_region_boundary(end_branch);
    printf("validated_flow_cases=%u\n", cases);
    return 0;
}
