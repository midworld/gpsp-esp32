/* Generated-code probes for the RV32I, M, and C instructions used by a JIT.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "probe.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "isa_probe";

typedef enum {
    CORE_I,
    CORE_M,
    CORE_C,
} core_group_t;

typedef enum {
    CORE_VALUE,
    CORE_LOAD,
    CORE_STORE,
    CORE_PC,
    CORE_LINK,
    CORE_JALR,
    CORE_CJR,
    CORE_CJALR,
    CORE_TRAP,
} core_kind_t;

#define CORE_SNIPPET(name) \
    extern const uint8_t core_##name##_start[]; \
    extern const uint8_t core_##name##_end[]
CORE_SNIPPET(add); CORE_SNIPPET(sub); CORE_SNIPPET(sll);
CORE_SNIPPET(slt); CORE_SNIPPET(sltu); CORE_SNIPPET(xor);
CORE_SNIPPET(srl); CORE_SNIPPET(sra); CORE_SNIPPET(or); CORE_SNIPPET(and);
CORE_SNIPPET(addi); CORE_SNIPPET(slti); CORE_SNIPPET(sltiu);
CORE_SNIPPET(xori); CORE_SNIPPET(ori); CORE_SNIPPET(andi);
CORE_SNIPPET(slli); CORE_SNIPPET(srli); CORE_SNIPPET(srai);
CORE_SNIPPET(lb); CORE_SNIPPET(lbu); CORE_SNIPPET(lh);
CORE_SNIPPET(lhu); CORE_SNIPPET(lw); CORE_SNIPPET(sb);
CORE_SNIPPET(sh); CORE_SNIPPET(sw);
CORE_SNIPPET(beq); CORE_SNIPPET(bne); CORE_SNIPPET(blt);
CORE_SNIPPET(bge); CORE_SNIPPET(bltu); CORE_SNIPPET(bgeu);
CORE_SNIPPET(lui); CORE_SNIPPET(auipc); CORE_SNIPPET(jal); CORE_SNIPPET(jalr);
CORE_SNIPPET(fence); CORE_SNIPPET(ecall); CORE_SNIPPET(ebreak);
CORE_SNIPPET(mul); CORE_SNIPPET(mulh); CORE_SNIPPET(mulhsu);
CORE_SNIPPET(mulhu); CORE_SNIPPET(div); CORE_SNIPPET(divu);
CORE_SNIPPET(rem); CORE_SNIPPET(remu);
CORE_SNIPPET(cli); CORE_SNIPPET(cmv); CORE_SNIPPET(cadd);
CORE_SNIPPET(csub); CORE_SNIPPET(cslli); CORE_SNIPPET(csrai);
CORE_SNIPPET(clw); CORE_SNIPPET(csw); CORE_SNIPPET(cj);
CORE_SNIPPET(cbeqz); CORE_SNIPPET(cbnez);
CORE_SNIPPET(caddi4spn); CORE_SNIPPET(cnop); CORE_SNIPPET(caddi);
CORE_SNIPPET(cjal); CORE_SNIPPET(caddi16sp); CORE_SNIPPET(clui);
CORE_SNIPPET(csrli); CORE_SNIPPET(candi); CORE_SNIPPET(cxor);
CORE_SNIPPET(cor); CORE_SNIPPET(cand); CORE_SNIPPET(clwsp);
CORE_SNIPPET(cswsp); CORE_SNIPPET(cjr); CORE_SNIPPET(cjalr);
CORE_SNIPPET(cebreak);
CORE_SNIPPET(ram_baseline);
#undef CORE_SNIPPET
extern const uint8_t core_jal_link[];
extern const uint8_t core_jalr_link[];
extern const uint8_t core_jalr_target[];
extern const uint8_t core_cjal_link[];
extern const uint8_t core_cjr_target[];
extern const uint8_t core_cjalr_link[];
extern const uint8_t core_cjalr_target[];

static portMUX_TYPE core_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    core_group_t group;
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
    uint32_t input;
    uint32_t arg;
    uint32_t expected;
    core_kind_t kind;
} core_case_t;

#define VALUE(group, name, input, arg, expected) \
    {group, #name, core_##name##_start, core_##name##_end, input, arg, expected, CORE_VALUE}
#define MEMORY(group, name, before, input, expected, kind) \
    {group, #name, core_##name##_start, core_##name##_end, input, before, expected, kind}

static const core_case_t cases[] = {
    VALUE(CORE_I, add, 0xffffffff, 2, 1),
    VALUE(CORE_I, sub, 0, 1, 0xffffffff),
    VALUE(CORE_I, sll, 1, 31, 0x80000000),
    VALUE(CORE_I, slt, 0x80000000, 1, 1),
    VALUE(CORE_I, sltu, 0x80000000, 1, 0),
    VALUE(CORE_I, xor, 0xaaaaaaaa, 0x55555555, 0xffffffff),
    VALUE(CORE_I, srl, 0x80000000, 31, 1),
    VALUE(CORE_I, sra, 0x80000000, 31, 0xffffffff),
    VALUE(CORE_I, or, 0xf000, 0xf, 0xf00f),
    VALUE(CORE_I, and, 0xf0, 0x0f, 0),
    VALUE(CORE_I, addi, 2047, 0, 0xffffffff),
    VALUE(CORE_I, slti, 0x80000000, 0, 1),
    VALUE(CORE_I, sltiu, 0, 0, 1),
    VALUE(CORE_I, xori, 0xaaaaaaaa, 0, 0x55555555),
    VALUE(CORE_I, ori, 0, 0, 0xfffff800),
    VALUE(CORE_I, andi, 0xffffffff, 0, 2047),
    VALUE(CORE_I, slli, 1, 0, 0x80000000),
    VALUE(CORE_I, srli, 0x80000000, 0, 1),
    VALUE(CORE_I, srai, 0x80000000, 0, 0xffffffff),
    MEMORY(CORE_I, lb, 0x7fff80fe, 0, 0xfffffffe, CORE_LOAD),
    MEMORY(CORE_I, lbu, 0x7fff80fe, 0, 0xfe, CORE_LOAD),
    MEMORY(CORE_I, lh, 0x7fff80fe, 0, 0xffff80fe, CORE_LOAD),
    MEMORY(CORE_I, lhu, 0x7fff80fe, 0, 0x80fe, CORE_LOAD),
    MEMORY(CORE_I, lw, 0x7fff80fe, 0, 0x7fff80fe, CORE_LOAD),
    MEMORY(CORE_I, sb, 0xaabbccdd, 0x12, 0x12bbccdd, CORE_STORE),
    MEMORY(CORE_I, sh, 0xaabbccdd, 0x1357, 0x1357ccdd, CORE_STORE),
    MEMORY(CORE_I, sw, 0xaabbccdd, 0x12345678, 0x12345678, CORE_STORE),
    VALUE(CORE_I, beq, 1, 1, 1),
    VALUE(CORE_I, beq, 1, 2, 0),
    VALUE(CORE_I, bne, 1, 2, 1),
    VALUE(CORE_I, bne, 1, 1, 0),
    VALUE(CORE_I, blt, 0x80000000, 1, 1),
    VALUE(CORE_I, blt, 1, 0x80000000, 0),
    VALUE(CORE_I, bge, 1, 0x80000000, 1),
    VALUE(CORE_I, bge, 0x80000000, 1, 0),
    VALUE(CORE_I, bltu, 1, 0x80000000, 1),
    VALUE(CORE_I, bltu, 0x80000000, 1, 0),
    VALUE(CORE_I, bgeu, 0x80000000, 1, 1),
    VALUE(CORE_I, bgeu, 1, 0x80000000, 0),
    VALUE(CORE_I, lui, 0, 0, 0x12345000),
    {CORE_I, "auipc", core_auipc_start, core_auipc_end, 0, 0, 0, CORE_PC},
    {CORE_I, "jal", core_jal_start, core_jal_end, 0, 0, 0, CORE_LINK},
    {CORE_I, "jalr", core_jalr_start, core_jalr_end, 0, 0, 0, CORE_JALR},
    VALUE(CORE_I, fence, 0x12345678, 0, 0x12345678),
    {CORE_I, "ecall", core_ecall_start, core_ecall_end, 0, 0, 11, CORE_TRAP},
    {CORE_I, "ebreak", core_ebreak_start, core_ebreak_end, 0, 0, 3, CORE_TRAP},

    VALUE(CORE_M, mul, 0x7fffffff, 2, 0xfffffffe),
    VALUE(CORE_M, mulh, 0x80000000, 2, 0xffffffff),
    VALUE(CORE_M, mulhsu, 0x80000000, 2, 0xffffffff),
    VALUE(CORE_M, mulhu, 0xffffffff, 2, 1),
    VALUE(CORE_M, div, 0x80000000, 0xffffffff, 0x80000000),
    VALUE(CORE_M, div, 7, 0, 0xffffffff),
    VALUE(CORE_M, divu, 0xffffffff, 2, 0x7fffffff),
    VALUE(CORE_M, divu, 7, 0, 0xffffffff),
    VALUE(CORE_M, rem, 0x80000000, 0xffffffff, 0),
    VALUE(CORE_M, rem, 7, 0, 7),
    VALUE(CORE_M, remu, 0xffffffff, 2, 1),
    VALUE(CORE_M, remu, 7, 0, 7),

    VALUE(CORE_C, cli, 0, 0, 0xffffffff),
    VALUE(CORE_C, cmv, 0, 0x12345678, 0x12345678),
    VALUE(CORE_C, cadd, 1, 2, 3),
    VALUE(CORE_C, csub, 5, 2, 3),
    VALUE(CORE_C, cslli, 1, 0, 0x80000000),
    VALUE(CORE_C, csrai, 0x80000000, 0, 0xffffffff),
    MEMORY(CORE_C, clw, 0x12345678, 0, 0x12345678, CORE_LOAD),
    MEMORY(CORE_C, csw, 0xaabbccdd, 0x12345678, 0x12345678, CORE_STORE),
    VALUE(CORE_C, cj, 0, 0, 1),
    VALUE(CORE_C, cbeqz, 0, 0, 1),
    VALUE(CORE_C, cbeqz, 2, 0, 0),
    VALUE(CORE_C, cbnez, 2, 0, 1),
    VALUE(CORE_C, cbnez, 0, 0, 0),
    VALUE(CORE_C, caddi4spn, 0, 0, 16),
    VALUE(CORE_C, cnop, 0, 0, 1),
    VALUE(CORE_C, caddi, 2, 0, 1),
    {CORE_C, "cjal", core_cjal_start, core_cjal_end, 0, 0, 0, CORE_LINK},
    VALUE(CORE_C, caddi16sp, 0, 0, 16),
    VALUE(CORE_C, clui, 0, 0, 0x1f000),
    VALUE(CORE_C, csrli, 0x80000000, 0, 1),
    VALUE(CORE_C, candi, 3, 0, 2),
    VALUE(CORE_C, cxor, 0xaaaa, 0x5555, 0xffff),
    VALUE(CORE_C, cor, 0xf000, 0xf, 0xf00f),
    VALUE(CORE_C, cand, 0xf0, 0x0f, 0),
    VALUE(CORE_C, clwsp, 0, 0x12345678, 0x12345678),
    VALUE(CORE_C, cswsp, 0x12345678, 0, 0x12345678),
    {CORE_C, "cjr", core_cjr_start, core_cjr_end, 0, 0, 1, CORE_CJR},
    {CORE_C, "cjalr", core_cjalr_start, core_cjalr_end, 0, 0, 0, CORE_CJALR},
    {CORE_C, "cebreak", core_cebreak_start, core_cebreak_end, 0, 0, 3, CORE_TRAP},
};

#undef VALUE
#undef MEMORY

static probe_status_t execute_case(uint8_t *code, const core_case_t *test)
{
    size_t length = (size_t)(test->end - test->start);
    if (!length || length > PROBE_CODE_SIZE)
    {
        ESP_LOGE(TAG, "Invalid core snippet: %s", test->name);
        return PROBE_FAIL;
    }

    memcpy(code, test->start, length);
    if (!probe_sync(code, PROBE_CODE_SIZE))
        return PROBE_FAIL;

    volatile uint32_t memory = test->arg;
    uint32_t input = test->input;
    uint32_t arg = test->arg;
    uint32_t expected = test->expected;
    if (test->kind == CORE_LOAD || test->kind == CORE_STORE)
        arg = (uint32_t)(uintptr_t)&memory;
    else if (test->kind == CORE_PC)
        expected = (uint32_t)(uintptr_t)code;
    else if (test->kind == CORE_LINK)
        expected = (uint32_t)(uintptr_t)(code +
                ((test->group == CORE_C ? core_cjal_link : core_jal_link) - test->start));
    else if (test->kind == CORE_JALR)
    {
        arg = ((uint32_t)(uintptr_t)(code +
                (core_jalr_target - test->start))) | 1;
        expected = (uint32_t)(uintptr_t)(code +
                (core_jalr_link - test->start));
    }
    else if (test->kind == CORE_CJR)
        arg = (uint32_t)(uintptr_t)(code + (core_cjr_target - test->start));
    else if (test->kind == CORE_CJALR)
    {
        arg = (uint32_t)(uintptr_t)(code + (core_cjalr_target - test->start));
        expected = (uint32_t)(uintptr_t)(code +
                (core_cjalr_link - test->start));
    }

    ESP_LOGI(TAG, "RUN %s (%u bytes)", test->name, (unsigned)length);
    uint32_t returned;
    probe_fault_t fault;
    portENTER_CRITICAL(&core_mux);
    bool executed = test->kind == CORE_TRAP ?
            probe_guarded_call_system(code, input, arg, 0, &returned, &fault) :
            probe_guarded_call(code, input, arg, 0, &returned, &fault);
    portEXIT_CRITICAL(&core_mux);
    if (test->kind == CORE_TRAP)
    {
        bool passed = !executed && fault.cause == expected &&
                fault.pc == (uint32_t)(uintptr_t)code;
        ESP_LOGI(TAG, "%s: %s cause=%lu expected=%lu pc=%08lx", test->name,
                passed ? "PASS" : "FAIL", (unsigned long)fault.cause,
                (unsigned long)expected, (unsigned long)fault.pc);
        return passed ? PROBE_PASS : PROBE_FAIL;
    }
    if (!executed)
    {
        ESP_LOGW(TAG, "%s: trap cause=%lu pc=%08lx tval=%08lx", test->name,
                (unsigned long)fault.cause, (unsigned long)fault.pc,
                (unsigned long)fault.tval);
        return PROBE_UNAVAILABLE;
    }
    uint32_t actual = test->kind == CORE_STORE ? memory : returned;
    bool passed = actual == expected;
    ESP_LOGI(TAG, "%s: %s actual=%08lx expected=%08lx", test->name,
            passed ? "PASS" : "FAIL", (unsigned long)actual,
            (unsigned long)expected);
    return passed ? PROBE_PASS : PROBE_FAIL;
}

probe_status_t probe_core_baseline(uint8_t *code)
{
    const core_case_t baseline = {
        CORE_I, "RAM addi baseline", core_ram_baseline_start,
        core_ram_baseline_end, 41, 0, 42, CORE_VALUE
    };
    return execute_case(code, &baseline);
}

static void run_group(uint8_t *code, core_group_t group)
{
    static const char *const group_names[] = {"RV32I", "RV32M", "RV32C"};
    unsigned passed = 0;
    unsigned unavailable = 0;
    unsigned failed = 0;
    unsigned total = 0;
    for (unsigned i = 0; i < PROBE_ARRAY_SIZE(cases); i++)
    {
        if (cases[i].group != group)
            continue;
        if (cases[i].kind == CORE_TRAP && cases[i].expected == 3)
        {
            ESP_LOGI(TAG, "SKIP %s: debugger/emulator may intercept EBREAK",
                    cases[i].name);
            continue;
        }
        total++;
        probe_status_t result = execute_case(code, &cases[i]);
        passed += result == PROBE_PASS;
        unavailable += result == PROBE_UNAVAILABLE;
        failed += result == PROBE_FAIL;
    }
    char detail[48];
    snprintf(detail, sizeof(detail), "%u/%u passed, %u trapped",
            passed, total, unavailable);
    probe_report(group_names[group], failed ? PROBE_FAIL :
            unavailable ? PROBE_UNAVAILABLE : PROBE_PASS,
            passed, total, unavailable ? detail : NULL);
}

void probe_core_run(uint8_t *code)
{
    for (unsigned group = CORE_I; group <= CORE_C; group++)
        run_group(code, group);
}

void probe_zca_run(uint8_t *code)
{
    run_group(code, CORE_C);
}
