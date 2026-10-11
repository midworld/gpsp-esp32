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
static portMUX_TYPE zcmp_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    uint32_t a0;
    uint32_t a1;
    uint32_t s0;
    uint32_t s1;
    uint32_t sp_delta;
    uint32_t expected_ra;
    uint32_t saved_ra;
    uint32_t saved_s0;
    uint32_t saved_s1;
} zcmp_result_t;

extern void zcmp_invoke(const void *code, zcmp_result_t *result,
        uint32_t arg0, uint32_t arg1);

typedef struct {
    const void *code;
    zcmp_result_t *result;
    uint32_t arg0;
    uint32_t arg1;
} zcmp_call_t;

__attribute__((noinline)) static uint32_t guarded_zcmp_bridge(uint32_t address,
        uint32_t unused1, uint32_t unused2)
{
    (void)unused1;
    (void)unused2;
    const zcmp_call_t *call = (const zcmp_call_t *)(uintptr_t)address;
    zcmp_invoke(call->code, call->result, call->arg0, call->arg1);
    return 0;
}

#define ZCMP_SNIPPET(name) \
    extern const uint8_t zcmp_##name##_start[]; \
    extern const uint8_t zcmp_##name##_end[]
ZCMP_SNIPPET(push);
ZCMP_SNIPPET(pop);
ZCMP_SNIPPET(pop_rev);
ZCMP_SNIPPET(popret);
ZCMP_SNIPPET(popret_rev);
ZCMP_SNIPPET(popretz);
ZCMP_SNIPPET(popretz_rev);
ZCMP_SNIPPET(roundtrip_pop);
ZCMP_SNIPPET(roundtrip_popret);
ZCMP_SNIPPET(roundtrip_popretz);
ZCMP_SNIPPET(mva01s);
ZCMP_SNIPPET(mvsa01);
#undef ZCMP_SNIPPET

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
    /* Pop forms only: the same pop with ra and s1 slots swapped. */
    const uint8_t *rev_start;
    const uint8_t *rev_end;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t expected_a0;
    uint32_t expected_a1;
    bool push;
} zcmp_case_t;

static const zcmp_case_t zcmp_cases[] = {
    {"cm.push {ra,s0-s1},-16", zcmp_push_start, zcmp_push_end, NULL, NULL,
        0, 0x80000001, 0, 0x80000001, true},
    {"cm.pop {ra,s0-s1},16", zcmp_pop_start, zcmp_pop_end,
        zcmp_pop_rev_start, zcmp_pop_rev_end,
        0x87654321, 0x80000001, 0x87654321, 0x80000001, false},
    {"cm.popret {ra,s0-s1},16", zcmp_popret_start, zcmp_popret_end,
        zcmp_popret_rev_start, zcmp_popret_rev_end,
        0x87654321, 0x80000001, 0x87654321, 0x80000001, false},
    {"cm.popretz {ra,s0-s1},16", zcmp_popretz_start, zcmp_popretz_end,
        zcmp_popretz_rev_start, zcmp_popretz_rev_end,
        0x87654321, 0x80000001, 0, 0x80000001, false},
    {"cm.push/pop roundtrip", zcmp_roundtrip_pop_start, zcmp_roundtrip_pop_end,
        NULL, NULL, 0x87654321, 0x80000001, 0x87654321, 0x80000001, false},
    {"cm.push/popret roundtrip", zcmp_roundtrip_popret_start,
        zcmp_roundtrip_popret_end, NULL, NULL,
        0x87654321, 0x80000001, 0x87654321, 0x80000001, false},
    {"cm.push/popretz roundtrip", zcmp_roundtrip_popretz_start,
        zcmp_roundtrip_popretz_end, NULL, NULL,
        0x87654321, 0x80000001, 0, 0x80000001, false},
    {"cm.mva01s s0,s1", zcmp_mva01s_start, zcmp_mva01s_end, NULL, NULL,
        0, 0, 0x13579bdf, 0x2468ace0, false},
    {"cm.mvsa01 s0,s1", zcmp_mvsa01_start, zcmp_mvsa01_end, NULL, NULL,
        0x87654321, 0x80000001, 0x87654321, 0x80000001, false},
};

/* PROBE_PASS with the spec frame layout; ZCMP_REVERSED with ra and s1 swapped. */
#define ZCMP_REVERSED PROBE_SKIPPED

static probe_status_t execute_zcmp_snippet(uint8_t *code,
        const zcmp_case_t *test, const uint8_t *start, const uint8_t *end,
        const char *layout)
{
    size_t length = (size_t)(end - start);
    if (!length || length > PROBE_CODE_SIZE)
    {
        ESP_LOGE(TAG, "Invalid Zcmp snippet size: %s (%u)", test->name,
                (unsigned)length);
        return PROBE_FAIL;
    }

    memcpy(code, start, length);
    if (!probe_sync(code, PROBE_CODE_SIZE))
        return PROBE_FAIL;

    zcmp_result_t result = {0};
    ESP_LOGI(TAG, "RUN %s%s (%u bytes)", test->name, layout, (unsigned)length);
    zcmp_call_t call = {
        .code = code,
        .result = &result,
        .arg0 = test->push ? (uint32_t)(uintptr_t)&result : test->arg0,
        .arg1 = test->arg1,
    };
    uint32_t ignored;
    probe_fault_t fault;
    portENTER_CRITICAL(&zcmp_mux);
    bool executed = probe_guarded_call(guarded_zcmp_bridge,
            (uint32_t)(uintptr_t)&call, 0, 0, &ignored, &fault);
    portEXIT_CRITICAL(&zcmp_mux);
    if (!executed)
    {
        ESP_LOGW(TAG, "%s%s: trap cause=%lu pc=%08lx tval=%08lx", test->name,
                layout, (unsigned long)fault.cause, (unsigned long)fault.pc,
                (unsigned long)fault.tval);
        return PROBE_UNAVAILABLE;
    }

    uint32_t expected_a0 = test->push ?
            (uint32_t)(uintptr_t)&result : test->expected_a0;
    bool passed = result.a0 == expected_a0 &&
            result.a1 == test->expected_a1 &&
            result.s0 == 0x13579bdf && result.s1 == 0x2468ace0 &&
            result.sp_delta == 0;
    probe_status_t status = passed ? PROBE_PASS : PROBE_FAIL;
    if (test->push && passed)
    {
        bool spec = result.saved_ra == result.expected_ra &&
                result.saved_s1 == 0x2468ace0;
        bool reversed = result.saved_ra == 0x2468ace0 &&
                result.saved_s1 == result.expected_ra;
        status = result.saved_s0 != 0x13579bdf ? PROBE_FAIL :
                spec ? PROBE_PASS : reversed ? ZCMP_REVERSED : PROBE_FAIL;
    }

    static const char *const names[] = {
        "PASS", "FAIL", "UNAVAILABLE", "PUSH_REVERSED"
    };
    ESP_LOGI(TAG, "%s%s: %s a0=%08lx a1=%08lx s0=%08lx s1=%08lx sp_delta=%ld",
            test->name, layout, names[status], (unsigned long)result.a0,
            (unsigned long)result.a1, (unsigned long)result.s0,
            (unsigned long)result.s1, (long)(int32_t)result.sp_delta);
    if (test->push)
        ESP_LOGI(TAG, "saved ra=%08lx expected=%08lx s0=%08lx s1=%08lx",
                (unsigned long)result.saved_ra,
                (unsigned long)result.expected_ra,
                (unsigned long)result.saved_s0,
                (unsigned long)result.saved_s1);
    return status;
}

static probe_status_t execute_zcmp_case(uint8_t *code, const zcmp_case_t *test)
{
    probe_status_t status = execute_zcmp_snippet(code, test, test->start,
            test->end, "");
    if (status == PROBE_PASS || !test->rev_start)
        return status;
    /* A spec-layout pop fails on reversed hardware; retry with ra/s1 swapped. */
    probe_status_t reversed = execute_zcmp_snippet(code, test,
            test->rev_start, test->rev_end, " (reversed frame)");
    return reversed == PROBE_PASS ? ZCMP_REVERSED : status;
}

void probe_zcmp_run(uint8_t *code)
{
    unsigned passed = 0;
    unsigned unavailable = 0;
    unsigned failed = 0;
    unsigned reversed = 0;
    for (unsigned i = 0; i < PROBE_ARRAY_SIZE(zcmp_cases); i++)
    {
        probe_status_t result = execute_zcmp_case(code, &zcmp_cases[i]);
        passed += result == PROBE_PASS;
        unavailable += result == PROBE_UNAVAILABLE;
        failed += result == PROBE_FAIL;
        reversed += result == ZCMP_REVERSED;
    }
    if (reversed && !failed && !unavailable)
    {
        ESP_LOGI(TAG, "ZCMP PROBE RESULT: PUSH_REVERSED (%u/%u spec, %u reversed frame)",
                passed, (unsigned)PROBE_ARRAY_SIZE(zcmp_cases), reversed);
        return;
    }
    char detail[64];
    snprintf(detail, sizeof(detail), "%u/%u passed, %u trapped, %u reversed",
            passed, (unsigned)PROBE_ARRAY_SIZE(zcmp_cases), unavailable, reversed);
    probe_report("ZCMP", failed ? PROBE_FAIL :
            unavailable ? PROBE_UNAVAILABLE : PROBE_PASS,
            passed, PROBE_ARRAY_SIZE(zcmp_cases),
            unavailable || reversed ? detail : NULL);
}
