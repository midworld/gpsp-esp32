/* Espressif PIE scalar and SIMD instruction probes.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "probe.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if ISA_PROBE_PIE_CASE < 0 || ISA_PROBE_PIE_CASE > 6
#error ISA_PROBE_PIE_CASE must be 0 through 6
#endif

static const char *TAG = "isa_probe";
extern uint32_t probe_xespv_baseline(uint32_t left, uint32_t right);
extern void probe_xespv_xorq_bare(void);
extern void probe_xespv_xorq_smoke(void);
extern void probe_xespv_vaddu8_smoke(void);
extern void probe_xespv_vld_smoke(void);
extern void probe_xespv_vst_smoke(void);
extern void probe_xespv_copy_smoke(void);
extern void probe_xespv_claim(const void *source);
extern void probe_xespv_save_q(void);
extern void probe_xespv_restore_q(void);
extern const uint8_t xespv_copy_start[];
extern const uint8_t xespv_copy_end[];
static portMUX_TYPE pie_mux = portMUX_INITIALIZER_UNLOCKED;

__attribute__((noinline)) static uint32_t guarded_pie_state(uint32_t unused0,
        uint32_t unused1, uint32_t unused2)
{
    (void)unused0;
    (void)unused1;
    (void)unused2;
    uint32_t state;
    __asm__ volatile("csrr %0, 0x7f2" : "=r"(state));
    return state;
}

typedef enum {
    SIMD_XOR,
    SIMD_ADD_SAT,
    SIMD_AND,
    SIMD_OR,
    SIMD_MAX,
    SIMD_MIN,
} simd_op_t;

static uint8_t expected_byte(simd_op_t op, uint8_t left, uint8_t right)
{
    switch (op)
    {
        case SIMD_XOR: return left ^ right;
        case SIMD_ADD_SAT:
        {
            unsigned sum = (unsigned)left + right;
            return (uint8_t)(sum > 255 ? 255 : sum);
        }
        case SIMD_AND: return left & right;
        case SIMD_OR: return left | right;
        case SIMD_MAX: return left > right ? left : right;
        case SIMD_MIN: return left < right ? left : right;
    }
    return 0;
}

static bool call_pie(const void *function, uint32_t arg0, uint32_t arg1,
        uint32_t arg2, uint32_t *result, probe_fault_t *fault,
        uint32_t *readback)
{
    uint8_t saved_q[48] __attribute__((aligned(16)));
    uint32_t ignored;
    probe_fault_t context_fault;
    uint32_t state;
    /* The task claimed PIE before entry; keep its context on this hart. */
    portENTER_CRITICAL(&pie_mux);
    __asm__ volatile("csrr %0, 0x7f2" : "=r"(state) :: "memory");
    if (!(state & 3u))
    {
        portEXIT_CRITICAL(&pie_mux);
        fault->cause = UINT32_MAX;
        fault->pc = 0;
        fault->tval = 0;
        *readback = state;
        return false;
    }
    if (!probe_guarded_call(probe_xespv_save_q,
            (uint32_t)(uintptr_t)saved_q, 0, 0, &ignored, &context_fault))
    {
        portEXIT_CRITICAL(&pie_mux);
        *fault = context_fault;
        *readback = state;
        return false;
    }
    bool executed = probe_guarded_call(function, arg0, arg1, arg2,
            result, fault);
    __asm__ volatile("csrr %0, 0x7f2" : "=r"(*readback) :: "memory");
    if (!probe_guarded_call(probe_xespv_restore_q,
            (uint32_t)(uintptr_t)saved_q, 0, 0, &ignored, &context_fault))
    {
        *fault = context_fault;
        executed = false;
    }
    portEXIT_CRITICAL(&pie_mux);
    return executed;
}

static const char *claim_pie(void)
{
    uint32_t state;
    probe_fault_t fault;
    portENTER_CRITICAL(&pie_mux);
    bool csr_ok = probe_guarded_call(guarded_pie_state, 0, 0, 0,
            &state, &fault);
    portEXIT_CRITICAL(&pie_mux);
    if (!csr_ok)
    {
        ESP_LOGW(TAG, "PIE state CSR trap cause=%lu pc=%08lx tval=%08lx",
                (unsigned long)fault.cause, (unsigned long)fault.pc,
                (unsigned long)fault.tval);
        return "PIE state CSR";
    }

    /* The first PIE opcode must reach IDF's lazy task ownership handler. */
    uint8_t source[16] __attribute__((aligned(16))) = {0};
    probe_xespv_claim(source);
    __asm__ volatile("csrr %0, 0x7f2" : "=r"(state));
    ESP_LOGI(TAG, "PIE context claim state=%lu on CPU%ld",
            (unsigned long)state, (long)xPortGetCoreID());
    return (state & 3u) != 0 ? NULL : "PIE context claim";
}

static probe_status_t run_pie_execution(const char *name, const void *function,
        uint32_t arg0)
{
    uint32_t ignored;
    uint32_t state;
    probe_fault_t fault;
    ESP_LOGI(TAG, "RUN PIE %s on CPU%ld", name,
            (long)xPortGetCoreID());
    bool executed = call_pie(function, arg0, 0, 0, &ignored, &fault, &state);
    if (!executed)
        ESP_LOGW(TAG, "PIE %s: trap cause=%lu pc=%08lx tval=%08lx pie_state=%08lx",
                name, (unsigned long)fault.cause, (unsigned long)fault.pc,
                (unsigned long)fault.tval, (unsigned long)state);
    else
        ESP_LOGI(TAG, "PIE %s: EXECUTED pie_state=%08lx", name,
                (unsigned long)state);
    return executed ? PROBE_PASS : PROBE_UNAVAILABLE;
}

static probe_status_t run_simd_arithmetic(const char *name, const void *function,
        simd_op_t op)
{
    uint8_t left[16] __attribute__((aligned(16)));
    uint8_t right[16] __attribute__((aligned(16)));
    uint8_t output[16] __attribute__((aligned(16))) = {0};
    uint8_t expected[16];
    for (unsigned i = 0; i < sizeof(left); i++)
    {
        left[i] = i == 0 ? 250 : (uint8_t)(i * 13);
        right[i] = i == 0 ? 10 : (uint8_t)(i * 7 + 1);
        expected[i] = expected_byte(op, left[i], right[i]);
    }

    uint32_t ignored;
    uint32_t state;
    probe_fault_t fault;
    ESP_LOGI(TAG, "RUN PIE %s on CPU%ld", name, (long)xPortGetCoreID());
    bool executed = call_pie(function, (uint32_t)(uintptr_t)left,
            (uint32_t)(uintptr_t)right, (uint32_t)(uintptr_t)output,
            &ignored, &fault, &state);
    if (!executed)
    {
        ESP_LOGW(TAG, "PIE %s: trap cause=%lu pc=%08lx tval=%08lx pie_state=%08lx",
                name, (unsigned long)fault.cause, (unsigned long)fault.pc,
                (unsigned long)fault.tval, (unsigned long)state);
        return PROBE_UNAVAILABLE;
    }

    bool passed = memcmp(output, expected, sizeof(expected)) == 0;
    ESP_LOGI(TAG, "PIE %s: %s first=%02x expected=%02x last=%02x expected=%02x pie_state=%08lx",
            name, passed ? "PASS" : "FAIL", output[0], expected[0],
            output[15], expected[15], (unsigned long)state);
    return passed ? PROBE_PASS : PROBE_FAIL;
}

static probe_status_t run_simd_copy(const char *location, const void *function)
{
    uint8_t source[16] __attribute__((aligned(16)));
    uint8_t output[16] __attribute__((aligned(16))) = {0};
    for (unsigned i = 0; i < sizeof(source); i++)
        source[i] = (uint8_t)(i * 17 + 3);

    uint32_t ignored;
    uint32_t state;
    probe_fault_t fault;
    ESP_LOGI(TAG, "RUN PIE 128-bit load/store roundtrip from %s on CPU%ld",
            location, (long)xPortGetCoreID());
    bool executed = call_pie(function,
            (uint32_t)(uintptr_t)source, (uint32_t)(uintptr_t)output, 0,
            &ignored, &fault, &state);
    if (!executed)
    {
        ESP_LOGW(TAG, "PIE %s load/store roundtrip: trap cause=%lu pc=%08lx "
                "tval=%08lx pie_state=%08lx",
                location, (unsigned long)fault.cause, (unsigned long)fault.pc,
                (unsigned long)fault.tval, (unsigned long)state);
        return PROBE_UNAVAILABLE;
    }

    bool passed = memcmp(source, output, sizeof(source)) == 0;
    ESP_LOGI(TAG, "PIE %s load/store roundtrip: %s first=%02x expected=%02x "
            "last=%02x expected=%02x pie_state=%08lx",
            location, passed ? "PASS" : "FAIL", output[0], source[0],
            output[15], source[15], (unsigned long)state);
    return passed ? PROBE_PASS : PROBE_FAIL;
}

static probe_status_t run_dynamic_copy(uint8_t *code)
{
    size_t length = (size_t)(xespv_copy_end - xespv_copy_start);
    if (!length || length > PROBE_CODE_SIZE)
    {
        ESP_LOGE(TAG, "Invalid PIE RAM copy snippet");
        return PROBE_FAIL;
    }
    memcpy(code, xespv_copy_start, length);
    if (!probe_sync(code, PROBE_CODE_SIZE))
    {
        ESP_LOGE(TAG, "PIE RAM copy cache synchronization failed");
        return PROBE_FAIL;
    }
    return run_simd_copy("RAM", code);
}

#define PIE_SNIPPET(name) \
    extern const uint8_t xespv_##name##_start[]; \
    extern const uint8_t xespv_##name##_end[]
PIE_SNIPPET(addx2);
PIE_SNIPPET(addx4);
PIE_SNIPPET(subx2);
PIE_SNIPPET(subx4);
PIE_SNIPPET(vaddu8);
PIE_SNIPPET(xorq);
PIE_SNIPPET(bare_xorq);
PIE_SNIPPET(andq);
PIE_SNIPPET(orq);
PIE_SNIPPET(vmaxu8);
PIE_SNIPPET(vminu8);
#undef PIE_SNIPPET

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
    uint32_t input;
    uint32_t arg;
    uint32_t expected;
    bool vector;
    simd_op_t op;
} pie_case_t;

static const pie_case_t cases[] = {
    {"esp.addx2", xespv_addx2_start, xespv_addx2_end,
        0xffffffff, 2, 3, false, SIMD_XOR},
    {"esp.addx4", xespv_addx4_start, xespv_addx4_end,
        1, 3, 13, false, SIMD_XOR},
    {"esp.subx2", xespv_subx2_start, xespv_subx2_end,
        1, 2, 0xfffffffd, false, SIMD_XOR},
    {"esp.subx4", xespv_subx4_start, xespv_subx4_end,
        0x80000000, 1, 0x7ffffffc, false, SIMD_XOR},
    {"esp.vadd.u8", xespv_vaddu8_start, xespv_vaddu8_end,
        0, 0, 0, true, SIMD_ADD_SAT},
    {"esp.xorq", xespv_xorq_start, xespv_xorq_end,
        0, 0, 0, true, SIMD_XOR},
    {"esp.andq", xespv_andq_start, xespv_andq_end,
        0, 0, 0, true, SIMD_AND},
    {"esp.orq", xespv_orq_start, xespv_orq_end,
        0, 0, 0, true, SIMD_OR},
    {"esp.vmax.u8", xespv_vmaxu8_start, xespv_vmaxu8_end,
        0, 0, 0, true, SIMD_MAX},
    {"esp.vmin.u8", xespv_vminu8_start, xespv_vminu8_end,
        0, 0, 0, true, SIMD_MIN},
};

static probe_status_t execute_case(uint8_t *code, const pie_case_t *test)
{
    size_t length = (size_t)(test->end - test->start);
    if (!length || length > PROBE_CODE_SIZE)
    {
        ESP_LOGE(TAG, "Invalid PIE snippet: %s", test->name);
        return PROBE_FAIL;
    }

    memcpy(code, test->start, length);
    if (!probe_sync(code, PROBE_CODE_SIZE))
        return PROBE_FAIL;

    uint8_t left[16] __attribute__((aligned(16)));
    uint8_t right[16] __attribute__((aligned(16)));
    uint8_t output[64] __attribute__((aligned(16))) = {0};
    uint8_t expected[16];
    for (unsigned i = 0; i < sizeof(left); i++)
    {
        left[i] = i == 0 ? 250 : (uint8_t)(i * 13);
        right[i] = i == 0 ? 10 : (uint8_t)(i * 7 + 1);
        expected[i] = expected_byte(test->op, left[i], right[i]);
    }

    ESP_LOGI(TAG, "RUN %s (%u bytes)", test->name, (unsigned)length);
    uint32_t returned = 0;
    uint32_t state;
    probe_fault_t fault;
    uint32_t arg0 = test->vector ? (uint32_t)(uintptr_t)left : test->input;
    uint32_t arg1 = test->vector ? (uint32_t)(uintptr_t)right : test->arg;
    uint32_t arg2 = test->vector ? (uint32_t)(uintptr_t)output : 0;
    if (!call_pie(code, arg0, arg1, arg2, &returned, &fault, &state))
    {
        ESP_LOGW(TAG, "%s: trap cause=%lu pc=%08lx tval=%08lx pie_state=%08lx",
                test->name, (unsigned long)fault.cause, (unsigned long)fault.pc,
                (unsigned long)fault.tval, (unsigned long)state);
        return PROBE_UNAVAILABLE;
    }

    bool passed = test->vector ?
            memcmp(output, expected, sizeof(expected)) == 0 :
            returned == test->expected;
    if (test->vector)
        ESP_LOGI(TAG, "%s: %s first=%02x expected=%02x last=%02x expected=%02x pie_state=%08lx",
                test->name, passed ? "PASS" : "FAIL", output[0], expected[0],
                output[15], expected[15], (unsigned long)state);
    else
        ESP_LOGI(TAG, "%s: %s actual=%08lx expected=%08lx pie_state=%08lx", test->name,
                passed ? "PASS" : "FAIL", (unsigned long)returned,
                (unsigned long)test->expected, (unsigned long)state);
    return passed ? PROBE_PASS : PROBE_FAIL;
}

static void count_status(probe_status_t status, unsigned *passed,
        unsigned *trapped, unsigned *failed)
{
    *passed += status == PROBE_PASS;
    *trapped += status == PROBE_UNAVAILABLE;
    *failed += status == PROBE_FAIL;
}

static probe_status_t run_scalar_baseline(void)
{
    ESP_LOGI(TAG, "RUN PIE scalar baseline on CPU%ld", (long)xPortGetCoreID());
    uint32_t baseline = 0;
    uint32_t state;
    probe_fault_t fault;
    bool executed = call_pie(probe_xespv_baseline, 0xffffffff, 2, 0,
            &baseline, &fault, &state);
    if (!executed)
    {
        ESP_LOGW(TAG, "PIE baseline: trap cause=%lu pc=%08lx tval=%08lx pie_state=%08lx",
                (unsigned long)fault.cause, (unsigned long)fault.pc,
                (unsigned long)fault.tval, (unsigned long)state);
        return PROBE_UNAVAILABLE;
    }
    ESP_LOGI(TAG, "PIE baseline: %s actual=%08lx expected=00000003 pie_state=%08lx",
            baseline == 3 ? "PASS" : "FAIL", (unsigned long)baseline,
            (unsigned long)state);
    return baseline == 3 ? PROBE_PASS : PROBE_FAIL;
}

void probe_xespv_run(uint8_t *code)
{
    const char *claim_failure = claim_pie();
    if (claim_failure)
    {
        probe_report("XESPV", PROBE_UNAVAILABLE, 0, 0, claim_failure);
        return;
    }
    if (ISA_PROBE_PIE_CASE == 2)
    {
        probe_status_t result = run_scalar_baseline();
        probe_report("XESPV", result, result == PROBE_PASS, 1, "flash esp.addx2");
        return;
    }
    if (ISA_PROBE_PIE_CASE >= 3)
    {
        unsigned index = ISA_PROBE_PIE_CASE - 3;
        probe_status_t result = execute_case(code, &cases[index]);
        probe_report("XESPV", result, result == PROBE_PASS, 1, cases[index].name);
        return;
    }

    uint8_t payload[16] __attribute__((aligned(16))) = {0};
    unsigned smoke_passed = 0;
    unsigned trapped = 0;
    unsigned failed = 0;
    probe_status_t status;
    status = run_pie_execution("esp.xorq bare flash", probe_xespv_xorq_bare, 0);
    count_status(status, &smoke_passed, &trapped, &failed);
    size_t bare_length = (size_t)(xespv_bare_xorq_end - xespv_bare_xorq_start);
    if (bare_length && bare_length <= PROBE_CODE_SIZE)
    {
        memcpy(code, xespv_bare_xorq_start, bare_length);
        status = probe_sync(code, PROBE_CODE_SIZE) ?
                run_pie_execution("esp.xorq bare RAM", code, 0) : PROBE_FAIL;
    }
    else
        status = PROBE_FAIL;
    count_status(status, &smoke_passed, &trapped, &failed);
    status = run_simd_arithmetic("esp.xorq", probe_xespv_xorq_smoke, SIMD_XOR);
    count_status(status, &smoke_passed, &trapped, &failed);
    status = run_simd_arithmetic("esp.vadd.u8", probe_xespv_vaddu8_smoke, SIMD_ADD_SAT);
    count_status(status, &smoke_passed, &trapped, &failed);
    status = run_pie_execution("esp.vld.128.ip", probe_xespv_vld_smoke,
            (uint32_t)(uintptr_t)payload);
    count_status(status, &smoke_passed, &trapped, &failed);
    status = run_pie_execution("esp.vst.128.ip", probe_xespv_vst_smoke,
            (uint32_t)(uintptr_t)payload);
    count_status(status, &smoke_passed, &trapped, &failed);
    status = run_simd_copy("flash", probe_xespv_copy_smoke);
    count_status(status, &smoke_passed, &trapped, &failed);
    status = run_dynamic_copy(code);
    count_status(status, &smoke_passed, &trapped, &failed);

    unsigned ram_passed = 0;
    unsigned ram_total = ISA_PROBE_PIE_CASE == 1 ?
            PROBE_ARRAY_SIZE(cases) - 4 : PROBE_ARRAY_SIZE(cases);
    for (unsigned i = 4; i < PROBE_ARRAY_SIZE(cases); i++)
    {
        probe_status_t result = execute_case(code, &cases[i]);
        count_status(result, &ram_passed, &trapped, &failed);
    }
    if (ISA_PROBE_PIE_CASE == 0)
        for (unsigned i = 0; i < 4; i++)
        {
            probe_status_t result = execute_case(code, &cases[i]);
            count_status(result, &ram_passed, &trapped, &failed);
        }

    unsigned baseline_passed = 0;
    if (ISA_PROBE_PIE_CASE == 0)
    {
        status = run_scalar_baseline();
        count_status(status, &baseline_passed, &trapped, &failed);
    }
    char detail[96];
    if (ISA_PROBE_PIE_CASE == 1)
        snprintf(detail, sizeof(detail), "smoke %u/8, RAM vector %u/%u, %u trapped",
                smoke_passed, ram_passed, ram_total, trapped);
    else
        snprintf(detail, sizeof(detail), "smoke %u/8, RAM %u/%u, scalar flash %u/1, %u trapped",
                smoke_passed, ram_passed, ram_total, baseline_passed, trapped);
    probe_report("XESPV", failed ? PROBE_FAIL :
            trapped ? PROBE_UNAVAILABLE : PROBE_PASS,
            smoke_passed + baseline_passed + ram_passed,
            8 + ram_total + (ISA_PROBE_PIE_CASE == 0 ? 1u : 0u), detail);
}
