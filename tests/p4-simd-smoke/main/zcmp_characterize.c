/* Compare Zcmp stack behavior with the ratified register order.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "probe.h"

#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
extern void esp_hw_stack_guard_monitor_start(void);
extern void esp_hw_stack_guard_monitor_stop(void);
#endif

static const char *TAG = "zcmp_char";
static portMUX_TYPE zcmp_char_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    uint32_t frame[28];
    uint32_t slots[28];
    uint32_t return_pc;
    uint32_t entry_sp;
    uint32_t exit_sp;
    uint32_t push_sp;
    uint32_t loaded_ra;
    uint32_t after_s[12];
    uint32_t after_a0;
} zcmp_char_result_t;

_Static_assert(offsetof(zcmp_char_result_t, slots) == 112, "slots offset");
_Static_assert(offsetof(zcmp_char_result_t, return_pc) == 224, "return_pc offset");
_Static_assert(offsetof(zcmp_char_result_t, entry_sp) == 228, "entry_sp offset");
_Static_assert(offsetof(zcmp_char_result_t, exit_sp) == 232, "exit_sp offset");
_Static_assert(offsetof(zcmp_char_result_t, push_sp) == 236, "push_sp offset");
_Static_assert(offsetof(zcmp_char_result_t, loaded_ra) == 240, "loaded_ra offset");
_Static_assert(offsetof(zcmp_char_result_t, after_s) == 244, "after_s offset");
_Static_assert(offsetof(zcmp_char_result_t, after_a0) == 292, "after_a0 offset");

extern void zcmp_char_invoke(const void *code, zcmp_char_result_t *result);
extern const uint8_t zcmp_char_fault_ret_ra_start[];
extern const uint8_t zcmp_char_fault_ret_ra_end[];
extern const uint8_t zcmp_char_fault_ret_ra_op[];
extern const uint8_t zcmp_char_fault_retz_ra_start[];
extern const uint8_t zcmp_char_fault_retz_ra_end[];
extern const uint8_t zcmp_char_fault_retz_ra_op[];

#define ZCMP_CHAR_SNIPPET(name) \
    extern const uint8_t zcmp_char_push_##name##_start[]; \
    extern const uint8_t zcmp_char_push_##name##_end[]; \
    extern const uint8_t zcmp_char_pop_##name##_start[]; \
    extern const uint8_t zcmp_char_pop_##name##_end[]
ZCMP_CHAR_SNIPPET(ra16);
ZCMP_CHAR_SNIPPET(ra32);
ZCMP_CHAR_SNIPPET(ra64);
ZCMP_CHAR_SNIPPET(s0_16);
ZCMP_CHAR_SNIPPET(s0_32);
ZCMP_CHAR_SNIPPET(s0_64);
ZCMP_CHAR_SNIPPET(s1_16);
ZCMP_CHAR_SNIPPET(s1_64);
ZCMP_CHAR_SNIPPET(s2_16);
ZCMP_CHAR_SNIPPET(s2_64);
ZCMP_CHAR_SNIPPET(s3_32);
ZCMP_CHAR_SNIPPET(s3_48);
ZCMP_CHAR_SNIPPET(s3_80);
ZCMP_CHAR_SNIPPET(s4_32);
ZCMP_CHAR_SNIPPET(s4_80);
ZCMP_CHAR_SNIPPET(s5_32);
ZCMP_CHAR_SNIPPET(s5_80);
ZCMP_CHAR_SNIPPET(s6_32);
ZCMP_CHAR_SNIPPET(s6_80);
ZCMP_CHAR_SNIPPET(s7_48);
ZCMP_CHAR_SNIPPET(s7_96);
ZCMP_CHAR_SNIPPET(s8_48);
ZCMP_CHAR_SNIPPET(s8_96);
ZCMP_CHAR_SNIPPET(s9_48);
ZCMP_CHAR_SNIPPET(s9_96);
ZCMP_CHAR_SNIPPET(s11_64);
ZCMP_CHAR_SNIPPET(s11_112);
#undef ZCMP_CHAR_SNIPPET

typedef struct {
    const char *name;
    unsigned saved_s;
    unsigned frame_size;
    const uint8_t *push_start;
    const uint8_t *push_end;
    const uint8_t *pop_start;
    const uint8_t *pop_end;
} zcmp_char_case_t;

#define ZCMP_CHAR_CASE(name, count, bytes) \
    {#name, count, bytes, zcmp_char_push_##name##_start, \
        zcmp_char_push_##name##_end, zcmp_char_pop_##name##_start, \
        zcmp_char_pop_##name##_end}
static const zcmp_char_case_t cases[] = {
    ZCMP_CHAR_CASE(ra16, 0, 16),
    ZCMP_CHAR_CASE(ra32, 0, 32),
    ZCMP_CHAR_CASE(ra64, 0, 64),
    ZCMP_CHAR_CASE(s0_16, 1, 16),
    ZCMP_CHAR_CASE(s0_32, 1, 32),
    ZCMP_CHAR_CASE(s0_64, 1, 64),
    ZCMP_CHAR_CASE(s1_16, 2, 16),
    ZCMP_CHAR_CASE(s1_64, 2, 64),
    ZCMP_CHAR_CASE(s2_16, 3, 16),
    ZCMP_CHAR_CASE(s2_64, 3, 64),
    ZCMP_CHAR_CASE(s3_32, 4, 32),
    ZCMP_CHAR_CASE(s3_48, 4, 48),
    ZCMP_CHAR_CASE(s3_80, 4, 80),
    ZCMP_CHAR_CASE(s4_32, 5, 32),
    ZCMP_CHAR_CASE(s4_80, 5, 80),
    ZCMP_CHAR_CASE(s5_32, 6, 32),
    ZCMP_CHAR_CASE(s5_80, 6, 80),
    ZCMP_CHAR_CASE(s6_32, 7, 32),
    ZCMP_CHAR_CASE(s6_80, 7, 80),
    ZCMP_CHAR_CASE(s7_48, 8, 48),
    ZCMP_CHAR_CASE(s7_96, 8, 96),
    ZCMP_CHAR_CASE(s8_48, 9, 48),
    ZCMP_CHAR_CASE(s8_96, 9, 96),
    ZCMP_CHAR_CASE(s9_48, 10, 48),
    ZCMP_CHAR_CASE(s9_96, 10, 96),
    ZCMP_CHAR_CASE(s11_64, 12, 64),
    ZCMP_CHAR_CASE(s11_112, 12, 112),
};
#undef ZCMP_CHAR_CASE

#define ZCMP_RET_LIST(M) \
    M(ra_16, 0, 16) \
    M(ra_32, 0, 32) \
    M(ra_48, 0, 48) \
    M(ra_64, 0, 64) \
    M(s0_16, 1, 16) \
    M(s0_32, 1, 32) \
    M(s0_48, 1, 48) \
    M(s0_64, 1, 64) \
    M(s1_16, 2, 16) \
    M(s1_32, 2, 32) \
    M(s1_48, 2, 48) \
    M(s1_64, 2, 64) \
    M(s2_16, 3, 16) \
    M(s2_32, 3, 32) \
    M(s2_48, 3, 48) \
    M(s2_64, 3, 64) \
    M(s3_32, 4, 32) \
    M(s3_48, 4, 48) \
    M(s3_64, 4, 64) \
    M(s3_80, 4, 80) \
    M(s4_32, 5, 32) \
    M(s4_48, 5, 48) \
    M(s4_64, 5, 64) \
    M(s4_80, 5, 80) \
    M(s5_32, 6, 32) \
    M(s5_48, 6, 48) \
    M(s5_64, 6, 64) \
    M(s5_80, 6, 80) \
    M(s6_32, 7, 32) \
    M(s6_48, 7, 48) \
    M(s6_64, 7, 64) \
    M(s6_80, 7, 80) \
    M(s7_48, 8, 48) \
    M(s7_64, 8, 64) \
    M(s7_80, 8, 80) \
    M(s7_96, 8, 96) \
    M(s8_48, 9, 48) \
    M(s8_64, 9, 64) \
    M(s8_80, 9, 80) \
    M(s8_96, 9, 96) \
    M(s9_48, 10, 48) \
    M(s9_64, 10, 64) \
    M(s9_80, 10, 80) \
    M(s9_96, 10, 96) \
    M(s11_64, 12, 64) \
    M(s11_80, 12, 80) \
    M(s11_96, 12, 96) \
    M(s11_112, 12, 112)

#define ZCMP_RET_SNIPPET(name, count, frame_size) \
    extern const uint8_t zcmp_char_ret_##name##_start[]; \
    extern const uint8_t zcmp_char_ret_##name##_end[]; \
    extern const uint8_t zcmp_char_retz_##name##_start[]; \
    extern const uint8_t zcmp_char_retz_##name##_end[]; \
    extern const uint8_t zcmp_char_loadret_##name##_start[]; \
    extern const uint8_t zcmp_char_loadret_##name##_end[]; \
    extern const uint8_t zcmp_char_loadretz_##name##_start[]; \
    extern const uint8_t zcmp_char_loadretz_##name##_end[];
ZCMP_RET_LIST(ZCMP_RET_SNIPPET)
#undef ZCMP_RET_SNIPPET

typedef struct {
    const char *name;
    unsigned saved_s;
    unsigned frame_size;
    const uint8_t *roundtrip_start;
    const uint8_t *roundtrip_end;
    const uint8_t *preload_start;
    const uint8_t *preload_end;
    bool zero_a0;
} zcmp_ret_case_t;

#define ZCMP_RET_CASE(name, count, bytes, variant, load_variant, zero) \
    {#name " " #variant, count, bytes, zcmp_char_##variant##_##name##_start, \
        zcmp_char_##variant##_##name##_end, \
        zcmp_char_##load_variant##_##name##_start, \
        zcmp_char_##load_variant##_##name##_end, zero}
#define ZCMP_RET_CASES(name, count, bytes) \
    ZCMP_RET_CASE(name, count, bytes, ret, loadret, false), \
    ZCMP_RET_CASE(name, count, bytes, retz, loadretz, true),
static const zcmp_ret_case_t ret_cases[] = {
    ZCMP_RET_LIST(ZCMP_RET_CASES)
};
_Static_assert(PROBE_ARRAY_SIZE(ret_cases) == 96, "Zcmp return encodings");
#undef ZCMP_RET_CASES
#undef ZCMP_RET_CASE
#undef ZCMP_RET_LIST

typedef enum {
    ORDER_OTHER,
    ORDER_SPEC,
    ORDER_REVERSED,
    ORDER_BOTH,
} zcmp_order_t;

static const char *order_name(zcmp_order_t order)
{
    static const char *const names[] = {"other", "spec", "reversed", "both"};
    return names[order];
}

static uint32_t seed_s(unsigned index)
{
    return 0x51500000 + index;
}

static zcmp_order_t push_order(const zcmp_char_case_t *test,
        const zcmp_char_result_t *result)
{
    unsigned words = test->frame_size / 4;
    unsigned saved = test->saved_s + 1;
    unsigned base = words - saved;
    bool spec = result->push_sp == result->entry_sp - test->frame_size;
    bool reversed = spec;

    for (unsigned i = 0; i < words; i++)
    {
        if (i < base)
            continue;
        unsigned index = i - base;
        uint32_t spec_value = index ? seed_s(index - 1) : result->return_pc;
        uint32_t reverse_value = index == saved - 1 ? result->return_pc :
                seed_s(saved - 2 - index);
        spec = spec && result->slots[i] == spec_value;
        reversed = reversed && result->slots[i] == reverse_value;
    }
    if (result->exit_sp != result->entry_sp)
        return ORDER_OTHER;
    for (unsigned i = 0; i < 12; i++)
        if (result->after_s[i] != seed_s(i))
            return ORDER_OTHER;
    return spec && reversed ? ORDER_BOTH :
            spec ? ORDER_SPEC : reversed ? ORDER_REVERSED : ORDER_OTHER;
}

static zcmp_order_t pop_order(const zcmp_char_case_t *test,
        const zcmp_char_result_t *result)
{
    unsigned saved = test->saved_s + 1;
    unsigned base = test->frame_size / 4 - saved;
    bool spec = result->loaded_ra == result->frame[base];
    bool reversed = result->loaded_ra == result->frame[base + saved - 1];

    for (unsigned i = 0; i < 12; i++)
    {
        uint32_t spec_value = i < test->saved_s ?
                result->frame[base + 1 + i] : seed_s(i);
        uint32_t reverse_value = i < test->saved_s ?
                result->frame[base + test->saved_s - 1 - i] : seed_s(i);
        spec = spec && result->after_s[i] == spec_value;
        reversed = reversed && result->after_s[i] == reverse_value;
    }
    if (result->exit_sp != result->entry_sp)
        return ORDER_OTHER;
    return spec && reversed ? ORDER_BOTH :
            spec ? ORDER_SPEC : reversed ? ORDER_REVERSED : ORDER_OTHER;
}

static bool run_snippet(const uint8_t *start, const uint8_t *end,
        uint8_t *code, bool ram, zcmp_char_result_t *result)
{
    size_t length = (size_t)(end - start);
    if (!length || length > PROBE_CODE_SIZE)
    {
        ESP_LOGE(TAG, "Invalid snippet length: %u", (unsigned)length);
        return false;
    }
    if (ram)
    {
        memcpy(code, start, length);
        if (!probe_sync(code, PROBE_CODE_SIZE))
            return false;
    }

    uint32_t ignored;
    probe_fault_t fault;
    portENTER_CRITICAL(&zcmp_char_mux);
    bool executed = probe_guarded_call((const void *)zcmp_char_invoke,
            (uint32_t)(uintptr_t)(ram ? code : start),
            (uint32_t)(uintptr_t)result, 0, &ignored, &fault);
    portEXIT_CRITICAL(&zcmp_char_mux);
    if (!executed)
        ESP_LOGW(TAG, "trap cause=%lu pc=%08lx tval=%08lx",
                (unsigned long)fault.cause, (unsigned long)fault.pc,
                (unsigned long)fault.tval);
    return executed;
}

static bool ret_passed(const zcmp_ret_case_t *test,
        const zcmp_char_result_t *result, bool preload)
{
    if (result->exit_sp != result->entry_sp ||
            result->after_a0 != (test->zero_a0 ? 0 :
                    (uint32_t)(uintptr_t)result))
        return false;
    for (unsigned i = 0; i < 12; i++)
    {
        uint32_t expected = preload && i < test->saved_s ?
                result->frame[test->frame_size / 4 - 2 - i] : seed_s(i);
        if (result->after_s[i] != expected)
            return false;
    }
    return true;
}

static bool run_fault_case(const uint8_t *start, const uint8_t *end,
        const uint8_t *operation, uint8_t *code, bool ram,
        unsigned core, const char *name)
{
    size_t length = (size_t)(end - start);
    if (!length || length > PROBE_CODE_SIZE)
        return false;
    if (ram)
    {
        memcpy(code, start, length);
        if (!probe_sync(code, PROBE_CODE_SIZE))
            return false;
    }

    uint32_t ignored;
    probe_fault_t fault;
    const uint8_t *entry = ram ? code : start;
    uint32_t expected_pc = (uint32_t)(uintptr_t)(entry + (operation - start));
    portENTER_CRITICAL(&zcmp_char_mux);
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
    esp_hw_stack_guard_monitor_stop();
#endif
    bool executed = probe_guarded_call(entry, 0, 0, 0, &ignored, &fault);
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
    esp_hw_stack_guard_monitor_start();
#endif
    portEXIT_CRITICAL(&zcmp_char_mux);
    /* CLIC records privilege and interrupt state above the cause field. */
    bool passed = !executed && ((fault.cause & 0xfff) == 5 ||
            (fault.cause & 0xfff) == 13) &&
            fault.pc == expected_pc;
    ESP_LOGI(TAG, "core%u %s %s fault %s cause=%08lx pc=%08lx expected=%08lx tval=%08lx",
            core, ram ? "RAM" : "flash", name, passed ? "PASS" : "FAIL",
            (unsigned long)fault.cause, (unsigned long)fault.pc,
            (unsigned long)expected_pc, (unsigned long)fault.tval);
    return passed;
}

static void run_suite(unsigned core)
{
    uint8_t *code = heap_caps_aligned_alloc(PROBE_CODE_ALIGNMENT,
            PROBE_CODE_SIZE, MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    if (!code)
    {
        ESP_LOGE(TAG, "core%u executable RAM allocation failed", core);
        return;
    }

    unsigned spec = 0;
    unsigned reversed = 0;
    unsigned both = 0;
    unsigned other = 0;
    for (unsigned i = 0; i < PROBE_ARRAY_SIZE(cases); i++)
    {
        const zcmp_char_case_t *test = &cases[i];
        for (unsigned ram = 0; ram < 2; ram++)
        {
            zcmp_char_result_t push = {0};
            zcmp_char_result_t pop = {0};
            for (unsigned word = 0; word < test->frame_size / 4; word++)
                pop.frame[word] = 0xa5000000 + word;

            bool push_ok = run_snippet(test->push_start, test->push_end,
                    code, ram, &push);
            bool pop_ok = run_snippet(test->pop_start, test->pop_end,
                    code, ram, &pop);
            zcmp_order_t push_result = push_ok ? push_order(test, &push) : ORDER_OTHER;
            zcmp_order_t pop_result = pop_ok ? pop_order(test, &pop) : ORDER_OTHER;
            ESP_LOGI(TAG, "core%u %s %s push=%s pop=%s sp=%08lx ra_slot=%08lx",
                    core, ram ? "RAM" : "flash", test->name,
                    order_name(push_result), order_name(pop_result),
                    (unsigned long)push.push_sp,
                    (unsigned long)pop.loaded_ra);
            zcmp_order_t results[] = {push_result, pop_result};
            for (unsigned j = 0; j < PROBE_ARRAY_SIZE(results); j++)
            {
                spec += results[j] == ORDER_SPEC;
                reversed += results[j] == ORDER_REVERSED;
                both += results[j] == ORDER_BOTH;
                other += results[j] == ORDER_OTHER;
            }
        }
    }
    ESP_LOGI(TAG, "core%u summary spec=%u reversed=%u both=%u other=%u",
            core, spec, reversed, both, other);

    unsigned roundtrip_passed = 0;
    unsigned preload_passed = 0;
    for (unsigned i = 0; i < PROBE_ARRAY_SIZE(ret_cases); i++)
    {
        const zcmp_ret_case_t *test = &ret_cases[i];
        for (unsigned ram = 0; ram < 2; ram++)
        {
            zcmp_char_result_t result = {0};
            bool executed = run_snippet(test->roundtrip_start,
                    test->roundtrip_end, code, ram, &result);
            bool passed = executed && ret_passed(test, &result, false);
            roundtrip_passed += passed;
            if (!passed)
                ESP_LOGE(TAG, "core%u %s %s roundtrip FAIL", core,
                        ram ? "RAM" : "flash", test->name);

            memset(&result, 0, sizeof(result));
            for (unsigned word = 0; word < test->frame_size / 4; word++)
                result.frame[word] = 0xa5000000 + word;
            executed = run_snippet(test->preload_start, test->preload_end,
                    code, ram, &result);
            passed = executed && ret_passed(test, &result, true);
            preload_passed += passed;
            if (!passed)
                ESP_LOGE(TAG, "core%u %s %s preload FAIL", core,
                        ram ? "RAM" : "flash", test->name);
        }
    }
    ESP_LOGI(TAG, "core%u popret/popretz roundtrip=%u/%u preload=%u/%u",
            core, roundtrip_passed, (unsigned)(PROBE_ARRAY_SIZE(ret_cases) * 2),
            preload_passed, (unsigned)(PROBE_ARRAY_SIZE(ret_cases) * 2));

    unsigned fault_passed = 0;
    for (unsigned ram = 0; ram < 2; ram++)
    {
        fault_passed += run_fault_case(zcmp_char_fault_ret_ra_start,
                zcmp_char_fault_ret_ra_end, zcmp_char_fault_ret_ra_op,
                code, ram, core, "popret");
        fault_passed += run_fault_case(zcmp_char_fault_retz_ra_start,
                zcmp_char_fault_retz_ra_end, zcmp_char_fault_retz_ra_op,
                code, ram, core, "popretz");
    }
    ESP_LOGI(TAG, "core%u popret/popretz load fault=%u/4",
            core, fault_passed);
    heap_caps_free(code);
}

static void core1_task(void *arg)
{
    TaskHandle_t caller = (TaskHandle_t)arg;
    run_suite(1);
    xTaskNotifyGive(caller);
    vTaskDelete(NULL);
}

void zcmp_characterize_run(void)
{
    run_suite(0);
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    if (xTaskCreatePinnedToCore(core1_task, "zcmp_char", 8192, caller,
            5, NULL, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "core1 task creation failed");
        return;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
