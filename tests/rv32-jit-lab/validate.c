/* Four-instruction Thumb IR validation on a small RV32I executor.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rv32_thumb_ir_emit.h"
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
    MAX_STEPS = 256,
};

static const uint16_t opcodes[4] = {0x4200, 0xd000, 0x2102, 0x2203};

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
    unsigned entry_mask;
    unsigned entry;
    unsigned sequential_wait;
    unsigned nonsequential_wait;
    int32_t budget;
    uint32_t r0;
    unsigned n, z, c, v;
} test_case;

static void fail(const char *message, const test_case *test)
{
    fprintf(stderr, "%s: mask=%u entry=%u wait=%u/%u budget=%" PRId32
            " r0=%08" PRIx32 " NZCV=%u%u%u%u\n", message,
            test->entry_mask, test->entry, test->sequential_wait,
            test->nonsequential_wait, test->budget, test->r0,
            test->n, test->z, test->c, test->v);
    exit(1);
}

static int32_t sign_extend(uint32_t bits, unsigned width)
{
    uint32_t sign = 1u << (width - 1);
    return (int32_t)((bits ^ sign) - sign);
}

static uint32_t word_at(const uint8_t *code, uint32_t offset)
{
    uint32_t word;
    memcpy(&word, code + offset, sizeof(word));
    return word;
}

static void init_state(thumb_state *state, rv32_state *rv,
        const test_case *test)
{
    memset(state, 0, sizeof(*state));
    memset(rv, 0, sizeof(*rv));
    for (unsigned i = 0; i < 15; i++)
    {
        state->guest[i] = 0x19324567u ^ (0x01010101u * i);
        rv->regs[guest_reg_map[i]] = state->guest[i];
    }
    state->guest[0] = test->r0;
    rv->regs[guest_reg_map[0]] = test->r0;
    state->pc = START_PC + test->entry * 2;
    state->budget = test->budget;
    state->n = test->n;
    state->z = test->z;
    state->c = test->c;
    state->v = test->v;
    rv->regs[RA] = RETURN_PC;
    rv->regs[RV_BUDGET] = (uint32_t)test->budget;
    rv->regs[RV_PC] = state->pc;
    rv->regs[RV_FLAG_N] = test->n;
    rv->regs[RV_FLAG_Z] = test->z;
    rv->regs[RV_FLAG_C] = test->c;
    rv->regs[RV_FLAG_V] = test->v;
}

static exit_kind expected_exit(const test_case *test)
{
    if (test->budget <= 0)
        return EXIT_NATIVE_RESUME;
    unsigned prefix;
    switch (test->entry)
    {
        case 0: prefix = test->nonsequential_wait +
                3 * test->sequential_wait; break;
        case 1: prefix = test->nonsequential_wait +
                2 * test->sequential_wait; break;
        case 2: prefix = test->sequential_wait; break;
        default: prefix = 0; break;
    }
    return test->entry != 3 && (unsigned)test->budget <= prefix ?
            EXIT_COLD_STEP : EXIT_COMPLETE;
}

static void thumb_step(thumb_state *state, unsigned sequential_wait,
        unsigned nonsequential_wait)
{
    switch ((state->pc - START_PC) / 2)
    {
        case 0:
            state->n = state->guest[0] >> 31;
            state->z = state->guest[0] == 0;
            state->budget -= sequential_wait;
            state->pc += 2;
            break;
        case 1:
            /* The current gpSP Bcc timing charges N+S for either outcome. */
            state->budget -= sequential_wait + nonsequential_wait;
            state->pc = state->z ? START_PC + 6 : START_PC + 4;
            break;
        case 2:
            state->guest[1] = 2;
            state->n = state->z = 0;
            state->budget -= sequential_wait;
            state->pc += 2;
            break;
        case 3:
            state->guest[2] = 3;
            state->n = state->z = 0;
            state->budget -= sequential_wait;
            state->pc += 2;
            break;
        default:
            abort();
    }
}

static void reference_run(thumb_state *state, exit_kind kind,
        const test_case *test)
{
    if (kind == EXIT_NATIVE_RESUME)
        return;
    do
    {
        thumb_step(state, test->sequential_wait, test->nonsequential_wait);
    } while (kind == EXIT_COMPLETE && state->pc < START_PC + 8);
}

/* The cold target is outside the emitted bytes. Model its one-step contract. */
static void cold_bridge_step(rv32_state *rv, const test_case *test)
{
    switch (test->entry)
    {
        case 0:
            rv->regs[RV_FLAG_N] = rv->regs[guest_reg_map[0]] >> 31;
            rv->regs[RV_FLAG_Z] = rv->regs[guest_reg_map[0]] == 0;
            rv->regs[RV_BUDGET] -= test->sequential_wait;
            rv->regs[RV_PC] += 2;
            break;
        case 1:
            rv->regs[RV_BUDGET] -= test->sequential_wait + test->nonsequential_wait;
            rv->regs[RV_PC] = rv->regs[RV_FLAG_Z] ? START_PC + 6 : START_PC + 4;
            break;
        case 2:
            rv->regs[guest_reg_map[1]] = 2;
            rv->regs[RV_FLAG_N] = rv->regs[RV_FLAG_Z] = 0;
            rv->regs[RV_BUDGET] -= test->sequential_wait;
            rv->regs[RV_PC] += 2;
            break;
        default:
            abort();
    }
}

static void rv32_run(rv32_state *rv, const uint8_t *code,
        const gpsp_rv32_thumb_ir_layout *layout, const test_case *test)
{
    rv->native_pc = CODE_BASE + layout->entry_offset[test->entry];
    for (unsigned steps = 0; steps < MAX_STEPS; steps++)
    {
        uint32_t offset = rv->native_pc - CODE_BASE;
        if ((offset & 3) || offset >= layout->code_bytes)
            fail("native PC outside generated code", test);
        uint32_t instruction = word_at(code, offset);
        uint32_t opcode = instruction & 0x7f;
        uint32_t rd = (instruction >> 7) & 31;
        uint32_t funct3 = (instruction >> 12) & 7;
        uint32_t rs1 = (instruction >> 15) & 31;
        uint32_t rs2 = (instruction >> 20) & 31;
        uint32_t next_pc = rv->native_pc + 4;
        int32_t immediate = sign_extend(instruction >> 20, 12);

        switch (opcode)
        {
            case 0x03:
            {
                if (funct3 != 4)
                    fail("unexpected load", test);
                uint32_t address = rv->regs[rs1] + immediate;
                if (address == SEQ_ADDRESS)
                    rv->regs[rd] = test->sequential_wait;
                else if (address == NSEQ_ADDRESS)
                    rv->regs[rd] = test->nonsequential_wait;
                else
                    fail("unexpected wait-table address", test);
                break;
            }
            case 0x13:
                if (funct3 == 0)
                    rv->regs[rd] = rv->regs[rs1] + immediate;
                else if (funct3 == 1 && ((instruction >> 25) & 0x7f) == 0)
                    rv->regs[rd] = rv->regs[rs1] << (immediate & 31);
                else
                    fail("unexpected immediate operation", test);
                break;
            case 0x33:
                if (funct3 == 0 && ((instruction >> 25) & 0x7f) == 0)
                    rv->regs[rd] = rv->regs[rs1] + rv->regs[rs2];
                else if (funct3 == 0 && ((instruction >> 25) & 0x7f) == 0x20)
                    rv->regs[rd] = rv->regs[rs1] - rv->regs[rs2];
                else if (funct3 == 7 && ((instruction >> 25) & 0x7f) == 0)
                    rv->regs[rd] = rv->regs[rs1] & rv->regs[rs2];
                else
                    fail("unexpected register operation", test);
                break;
            case 0x37:
                rv->regs[rd] = instruction & 0xfffff000u;
                break;
            case 0x63:
            {
                uint32_t bits = ((instruction >> 31) << 12) |
                        (((instruction >> 7) & 1) << 11) |
                        (((instruction >> 25) & 0x3f) << 5) |
                        (((instruction >> 8) & 0xf) << 1);
                bool taken = false;
                if (funct3 == 0)
                    taken = rv->regs[rs1] == rv->regs[rs2];
                else if (funct3 == 1)
                    taken = rv->regs[rs1] != rv->regs[rs2];
                else if (funct3 == 4)
                    taken = (int32_t)rv->regs[rs1] < (int32_t)rv->regs[rs2];
                else
                    fail("unexpected branch", test);
                if (taken)
                    next_pc = rv->native_pc + sign_extend(bits, 13);
                break;
            }
            case 0x6f:
            {
                uint32_t bits = ((instruction >> 31) << 20) |
                        (((instruction >> 12) & 0xff) << 12) |
                        (((instruction >> 20) & 1) << 11) |
                        (((instruction >> 21) & 0x3ff) << 1);
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

static void compare_state(const thumb_state *expected, const rv32_state *rv,
        const test_case *test)
{
    for (unsigned i = 0; i < 15; i++)
        if (expected->guest[i] != rv->regs[guest_reg_map[i]])
        {
            fprintf(stderr, "r%u expected=%08" PRIx32 " actual=%08" PRIx32
                    "\n", i, expected->guest[i], rv->regs[guest_reg_map[i]]);
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

int main(void)
{
    gpsp_rv32_thumb_ir ir;
    if (!gpsp_rv32_thumb_ir_decode(START_PC, opcodes, 4, &ir))
    {
        fputs("IR decode failed\n", stderr);
        return 1;
    }

    const unsigned masks[] = {1, 15, 15, 15};
    const uint32_t r0_values[] = {0, 1, 0x80000000u};
    const unsigned waits[][2] = {{1, 1}, {3, 5}, {7, 2}};
    unsigned cases = 0;
    for (unsigned m = 0; m < sizeof(masks) / sizeof(masks[0]); m++)
    {
        uint32_t words[256];
        gpsp_rv32_code code;
        gpsp_rv32_code_init(&code, words, sizeof(words));
        gpsp_rv32_thumb_ir_layout layout;
        gpsp_rv32_thumb_ir_environment environment = {
            .sequential_wait_address = SEQ_ADDRESS,
            .nonsequential_wait_address = NSEQ_ADDRESS,
            .interpreter_stub_address = STUB_PC,
            .idle_loop_target_pc = UINT32_MAX,
            .entry_mask = m >= 2 ? 1 : masks[m],
        };
        if (!gpsp_rv32_thumb_ir_emit(&ir, &environment, &code, &layout) ||
            layout.code_bytes != (size_t)(code.ptr - code.start))
        {
            fputs("IR emission failed\n", stderr);
            return 1;
        }
        if (m >= 2)
        {
            const unsigned order[][3] = {{1, 2, 3}, {3, 1, 2}};
            if (m == 2)
            {
                gpsp_rv32_code short_code = code;
                gpsp_rv32_thumb_ir_layout old_layout = layout;
                short_code.end = short_code.ptr + 4;
                if (gpsp_rv32_thumb_ir_append_entry(&ir, &environment, 1,
                        &short_code, &layout) ||
                        memcmp(&layout, &old_layout, sizeof(layout)))
                {
                    fputs("short append changed the block\n", stderr);
                    return 1;
                }
            }
            for (unsigned i = 0; i < 3; i++)
                if (!gpsp_rv32_thumb_ir_append_entry(&ir, &environment,
                        order[m - 2][i], &code, &layout))
                {
                    fputs("side entry append failed\n", stderr);
                    return 1;
                }
            size_t before = layout.code_bytes;
            if (gpsp_rv32_thumb_ir_append_entry(&ir, &environment, 1,
                    &code, &layout) || layout.code_bytes != before)
            {
                fputs("duplicate side entry was accepted\n", stderr);
                return 1;
            }
        }
        for (unsigned entry = 0; entry < 4; entry++)
        {
            if (!(masks[m] & (1u << entry)))
            {
                if (layout.entry_offset[entry] != UINT16_MAX)
                {
                    fputs("omitted entry was published\n", stderr);
                    return 1;
                }
                continue;
            }
            if (layout.entry_offset[entry] >= layout.code_bytes)
            {
                fputs("entry outside emitted code\n", stderr);
                return 1;
            }
            for (unsigned w = 0; w < sizeof(waits) / sizeof(waits[0]); w++)
                for (unsigned value = 0;
                        value < sizeof(r0_values) / sizeof(r0_values[0]); value++)
                    for (unsigned flags = 0; flags < 16; flags++)
                        for (int budget = -1; budget <= 32; budget++)
                        {
                            test_case test = {
                                .entry_mask = masks[m],
                                .entry = entry,
                                .sequential_wait = waits[w][0],
                                .nonsequential_wait = waits[w][1],
                                .budget = budget,
                                .r0 = r0_values[value],
                                .n = (flags >> 3) & 1,
                                .z = (flags >> 2) & 1,
                                .c = (flags >> 1) & 1,
                                .v = flags & 1,
                            };
                            thumb_state expected;
                            thumb_state initial;
                            rv32_state rv;
                            init_state(&expected, &rv, &test);
                            initial = expected;
                            exit_kind exit = expected_exit(&test);
                            reference_run(&expected, exit, &test);
                            rv32_run(&rv, (const uint8_t *)words, &layout, &test);
                            if (exit != rv.exit)
                                fail("exit kind mismatch", &test);
                            if (exit == EXIT_COLD_STEP)
                            {
                                compare_state(&initial, &rv, &test);
                                cold_bridge_step(&rv, &test);
                            }
                            compare_state(&expected, &rv, &test);
                            cases++;
                        }
        }
        printf("entry_mask=%u code_bytes=%u cases=%u\n", masks[m],
                layout.code_bytes, cases);
    }
    printf("validated_cases=%u\n", cases);
    return 0;
}
