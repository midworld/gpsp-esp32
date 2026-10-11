/* Generated-code probes for RV32A atomics and RV32F floating point.
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
static portMUX_TYPE af_mux = portMUX_INITIALIZER_UNLOCKED;

#define AF_SNIPPET(name) \
    extern const uint8_t af_##name##_start[]; \
    extern const uint8_t af_##name##_end[]
AF_SNIPPET(lrsc);
AF_SNIPPET(amoadd);
AF_SNIPPET(amoswap);
AF_SNIPPET(fcopy);
AF_SNIPPET(fadd);
AF_SNIPPET(fmul);
AF_SNIPPET(fcvt);
#undef AF_SNIPPET

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
    uint32_t initial;
    uint32_t operand;
    uint32_t expected_return;
    uint32_t expected_memory;
} atomic_case_t;

static const atomic_case_t atomic_cases[] = {
    {"lr.w/sc.w", af_lrsc_start, af_lrsc_end,
        0x7fffffff, 0, 0, 0x80000000},
    {"amoadd.w", af_amoadd_start, af_amoadd_end,
        0x7fffffff, 1, 0x7fffffff, 0x80000000},
    {"amoswap.w", af_amoswap_start, af_amoswap_end,
        0xaabbccdd, 0x12345678, 0xaabbccdd, 0x12345678},
};

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
    uint32_t left;
    uint32_t right;
    uint32_t expected;
    bool returns_integer;
} float_case_t;

static const float_case_t float_cases[] = {
    {"flw/fsw", af_fcopy_start, af_fcopy_end,
        0xbf400000, 0, 0xbf400000, false},
    {"fadd.s", af_fadd_start, af_fadd_end,
        0x3fc00000, 0x40100000, 0x40700000, false},
    {"fmul.s", af_fmul_start, af_fmul_end,
        0x3fc00000, 0x40100000, 0x40580000, false},
    {"fcvt.w.s", af_fcvt_start, af_fcvt_end,
        0x40700000, 0, 3, true},
};

/* Claim this task's FPU context through the normal ESP-IDF lazy trap path. */
__attribute__((noinline)) static void claim_fpu(void)
{
    volatile float left = 1.0f;
    volatile float right = 2.0f;
    volatile float sum = left + right;
    (void)sum;
}

static probe_status_t call_from_ram(uint8_t *code, const char *name,
        const uint8_t *start, const uint8_t *end, uint32_t arg0,
        uint32_t arg1, uint32_t arg2, uint32_t *returned, bool fpu)
{
    size_t length = (size_t)(end - start);
    if (!length || length > PROBE_CODE_SIZE)
    {
        ESP_LOGE(TAG, "Invalid A/F snippet: %s", name);
        return PROBE_FAIL;
    }

    memcpy(code, start, length);
    if (!probe_sync(code, PROBE_CODE_SIZE))
        return PROBE_FAIL;

    ESP_LOGI(TAG, "RUN %s from RAM (%u bytes) on CPU%ld", name,
            (unsigned)length, (long)xPortGetCoreID());
    probe_fault_t fault;
    portENTER_CRITICAL(&af_mux);
    /* A context switch may turn the FPU off again; reclaim it with switches off. */
    if (fpu)
        claim_fpu();
    bool executed = probe_guarded_call(code, arg0, arg1, arg2,
            returned, &fault);
    portEXIT_CRITICAL(&af_mux);
    if (!executed)
    {
        ESP_LOGW(TAG, "%s: trap cause=%lu pc=%08lx tval=%08lx", name,
                (unsigned long)fault.cause, (unsigned long)fault.pc,
                (unsigned long)fault.tval);
        return PROBE_UNAVAILABLE;
    }
    return PROBE_PASS;
}

static void report_group(const char *name, unsigned passed,
        unsigned unavailable, unsigned failed, unsigned total)
{
    if (unavailable)
    {
        char detail[64];
        snprintf(detail, sizeof(detail), "%u/%u passed, %u trapped",
                passed, total, unavailable);
        probe_report(name, failed ? PROBE_FAIL : PROBE_UNAVAILABLE,
                passed, total, detail);
    }
    else
        probe_report(name, failed ? PROBE_FAIL : PROBE_PASS,
                passed, total, NULL);
}

static void run_atomic(uint8_t *code)
{
    unsigned passed = 0;
    unsigned unavailable = 0;
    unsigned failed = 0;
    for (unsigned i = 0; i < PROBE_ARRAY_SIZE(atomic_cases); i++)
    {
        const atomic_case_t *test = &atomic_cases[i];
        volatile uint32_t memory = test->initial;
        uint32_t returned = 0;
        probe_status_t status = call_from_ram(code, test->name,
                test->start, test->end, (uint32_t)(uintptr_t)&memory,
                test->operand, 0, &returned, false);
        if (status == PROBE_PASS)
        {
            bool correct = returned == test->expected_return &&
                    memory == test->expected_memory;
            ESP_LOGI(TAG, "%s: %s return=%08lx expected=%08lx memory=%08lx expected=%08lx",
                    test->name, correct ? "PASS" : "FAIL",
                    (unsigned long)returned, (unsigned long)test->expected_return,
                    (unsigned long)memory, (unsigned long)test->expected_memory);
            status = correct ? PROBE_PASS : PROBE_FAIL;
        }
        passed += status == PROBE_PASS;
        unavailable += status == PROBE_UNAVAILABLE;
        failed += status == PROBE_FAIL;
    }
    report_group("RV32A", passed, unavailable, failed,
            PROBE_ARRAY_SIZE(atomic_cases));
}

static void run_float(uint8_t *code)
{
    claim_fpu();
    uint32_t mstatus;
    __asm__ volatile("csrr %0, mstatus" : "=r"(mstatus));
    unsigned fs = (mstatus >> 13) & 3;
    ESP_LOGI(TAG, "FPU state=%u on CPU%ld", fs, (long)xPortGetCoreID());
    if (fs == 0)
    {
        probe_report("RV32F", PROBE_UNAVAILABLE, 0, 0, "FPU state OFF");
        return;
    }

    unsigned passed = 0;
    unsigned unavailable = 0;
    unsigned failed = 0;
    for (unsigned i = 0; i < PROBE_ARRAY_SIZE(float_cases); i++)
    {
        const float_case_t *test = &float_cases[i];
        uint32_t left = test->left;
        uint32_t right = test->right;
        volatile uint32_t output = 0xdeadbeef;
        uint32_t returned = 0;
        probe_status_t status = call_from_ram(code, test->name,
                test->start, test->end, (uint32_t)(uintptr_t)&left,
                (uint32_t)(uintptr_t)&right, (uint32_t)(uintptr_t)&output,
                &returned, true);
        if (status == PROBE_PASS)
        {
            uint32_t actual = test->returns_integer ? returned : output;
            bool correct = actual == test->expected;
            ESP_LOGI(TAG, "%s: %s actual=%08lx expected=%08lx", test->name,
                    correct ? "PASS" : "FAIL", (unsigned long)actual,
                    (unsigned long)test->expected);
            status = correct ? PROBE_PASS : PROBE_FAIL;
        }
        passed += status == PROBE_PASS;
        unavailable += status == PROBE_UNAVAILABLE;
        failed += status == PROBE_FAIL;
    }
    report_group("RV32F", passed, unavailable, failed,
            PROBE_ARRAY_SIZE(float_cases));
}

void probe_af_run(uint8_t *code)
{
    run_atomic(code);
    run_float(code);
}
