/* ISA instruction probes.
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
static portMUX_TYPE zcb_mux = portMUX_INITIALIZER_UNLOCKED;

typedef enum {
    RESULT_REGISTER,
    RESULT_MEMORY,
} result_kind_t;

typedef struct {
    const char *name;
    uint16_t instruction;
    uint32_t input;
    uint32_t arg;
    uint32_t memory_before;
    uint32_t expected;
    result_kind_t result_kind;
    bool memory_arg;
} probe_case_t;

/* These are the 16-bit encodings for the operands shown in each name.
 * Checked with riscv32-esp-elf-as -march=rv32imafc_zcb_zba_zbb_zbs.
 */
static const probe_case_t cases[] = {
    {"c.lbu a0,0(a1) low", 0x8188, 0, 0, 0x12345680, 0x80, RESULT_REGISTER, true},
    {"c.lbu a0,3(a1) high", 0x81e8, 0, 0, 0x12345680, 0x12, RESULT_REGISTER, true},
    {"c.lhu a0,0(a1) low", 0x8588, 0, 0, 0x1234fedc, 0xfedc, RESULT_REGISTER, true},
    {"c.lhu a0,2(a1) high", 0x85a8, 0, 0, 0x1234fedc, 0x1234, RESULT_REGISTER, true},
    {"c.lh a0,0(a1) negative", 0x85c8, 0, 0, 0x7fff8000, 0xffff8000, RESULT_REGISTER, true},
    {"c.lh a0,2(a1) positive", 0x85e8, 0, 0, 0x7fff8000, 0x7fff, RESULT_REGISTER, true},
    {"c.sb a0,0(a1) low", 0x8988, 0x80, 0, 0xaabbccdd, 0xaabbcc80, RESULT_MEMORY, true},
    {"c.sb a0,3(a1) high", 0x89e8, 0x12, 0, 0xaabbccdd, 0x12bbccdd, RESULT_MEMORY, true},
    {"c.sh a0,0(a1) low", 0x8d88, 0x8001, 0, 0xaabbccdd, 0xaabb8001, RESULT_MEMORY, true},
    {"c.sh a0,2(a1) high", 0x8da8, 0x7fff, 0, 0xaabbccdd, 0x7fffccdd, RESULT_MEMORY, true},
    {"c.zext.b a0 negative byte", 0x9d61, 0xffffff80, 0, 0, 0x80, RESULT_REGISTER, false},
    {"c.zext.b a0 all ones", 0x9d61, 0xffffffff, 0, 0, 0xff, RESULT_REGISTER, false},
    {"c.sext.b a0 negative", 0x9d65, 0x80, 0, 0, 0xffffff80, RESULT_REGISTER, false},
    {"c.sext.b a0 positive", 0x9d65, 0x7f, 0, 0, 0x7f, RESULT_REGISTER, false},
    {"c.zext.h a0 negative half", 0x9d69, 0xffff8000, 0, 0, 0x8000, RESULT_REGISTER, false},
    {"c.zext.h a0 all ones", 0x9d69, 0xffffffff, 0, 0, 0xffff, RESULT_REGISTER, false},
    {"c.sext.h a0 negative", 0x9d6d, 0x8000, 0, 0, 0xffff8000, RESULT_REGISTER, false},
    {"c.sext.h a0 positive", 0x9d6d, 0x7fff, 0, 0, 0x7fff, RESULT_REGISTER, false},
    {"c.not a0 zero", 0x9d75, 0, 0, 0, 0xffffffff, RESULT_REGISTER, false},
    {"c.not a0 pattern", 0x9d75, 0xaaaaaaaa, 0, 0, 0x55555555, RESULT_REGISTER, false},
    {"c.mul a0,a1 overflow", 0x9d4d, 0x7fffffff, 2, 0, 0xfffffffe, RESULT_REGISTER, false},
    {"c.mul a0,a1 wrap", 0x9d4d, 0x80000000, 2, 0, 0, RESULT_REGISTER, false},
    {"c.mul a0,a1 negative", 0x9d4d, 0xffffffff, 0xffffffff, 0, 1, RESULT_REGISTER, false},
};

static probe_status_t execute_case(uint8_t *code, const probe_case_t *test)
{
    static const uint32_t ret = 0x00008067; /* jalr zero, 0(ra) */
    volatile uint32_t memory = test->memory_before;
    uint32_t arg = test->memory_arg ?
            (uint32_t)(uintptr_t)&memory : test->arg;

    memcpy(code, &test->instruction, sizeof(test->instruction));
    memcpy(code + sizeof(test->instruction), &ret, sizeof(ret));

    if (!probe_sync(code, PROBE_CODE_SIZE))
        return PROBE_FAIL;

    ESP_LOGI(TAG, "RUN %s encoding=%04x", test->name, test->instruction);
    uint32_t returned;
    probe_fault_t fault;
    portENTER_CRITICAL(&zcb_mux);
    bool executed = probe_guarded_call(code, test->input, arg, 0,
            &returned, &fault);
    portEXIT_CRITICAL(&zcb_mux);
    if (!executed)
    {
        ESP_LOGW(TAG, "%s: trap cause=%lu pc=%08lx tval=%08lx", test->name,
                (unsigned long)fault.cause, (unsigned long)fault.pc,
                (unsigned long)fault.tval);
        return PROBE_UNAVAILABLE;
    }
    uint32_t actual = test->result_kind == RESULT_MEMORY ? memory : returned;
    bool passed = actual == test->expected;
    ESP_LOGI(TAG, "%s: %s actual=%08lx expected=%08lx", test->name,
            passed ? "PASS" : "FAIL", (unsigned long)actual,
            (unsigned long)test->expected);
    return passed ? PROBE_PASS : PROBE_FAIL;
}

probe_status_t probe_zcb_baseline(uint8_t *code)
{
    const probe_case_t base_case = {
        "c.addi a0,1 baseline", 0x0505, 41, 0, 0, 42, RESULT_REGISTER, false
    };
    return execute_case(code, &base_case);
}

void probe_zcb_run(uint8_t *code)
{
    unsigned passed = 0;
    unsigned unavailable = 0;
    unsigned failed = 0;
    for (unsigned i = 0; i < PROBE_ARRAY_SIZE(cases); i++)
    {
        probe_status_t result = execute_case(code, &cases[i]);
        passed += result == PROBE_PASS;
        unavailable += result == PROBE_UNAVAILABLE;
        failed += result == PROBE_FAIL;
    }
    if (unavailable)
    {
        char detail[64];
        snprintf(detail, sizeof(detail), "%u/%u passed, %u illegal",
                passed, (unsigned)PROBE_ARRAY_SIZE(cases), unavailable);
        probe_report("ZCB", failed ? PROBE_FAIL : PROBE_UNAVAILABLE,
                passed, PROBE_ARRAY_SIZE(cases), detail);
    }
    else
        probe_report("ZCB", failed ? PROBE_FAIL : PROBE_PASS,
                passed, PROBE_ARRAY_SIZE(cases), NULL);
}
