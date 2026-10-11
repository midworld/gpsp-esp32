/* Espressif hardware-loop execution and state probes.
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
static portMUX_TYPE loop_mux = portMUX_INITIALIZER_UNLOCKED;

#define LOOP_SNIPPET(name) \
    extern const uint8_t xesploop_##name##_start[]; \
    extern const uint8_t xesploop_##name##_end[]
LOOP_SNIPPET(setupi);
LOOP_SNIPPET(setup);
LOOP_SNIPPET(starti);
LOOP_SNIPPET(endi);
LOOP_SNIPPET(counti);
LOOP_SNIPPET(count);
LOOP_SNIPPET(flash_setupi);
LOOP_SNIPPET(flash_endi);
LOOP_SNIPPET(flash_counti);
#undef LOOP_SNIPPET

#define LOOP_BODY(name) \
    extern const uint8_t xesploop_##name##_body_start[]; \
    extern const uint8_t xesploop_##name##_body_end[]
LOOP_BODY(starti);
LOOP_BODY(endi);
LOOP_BODY(counti);
LOOP_BODY(count);
LOOP_BODY(flash_endi);
LOOP_BODY(flash_counti);
#undef LOOP_BODY

typedef enum {
    LOOP_SETUP,
    LOOP_START,
    LOOP_END,
    LOOP_COUNT,
} loop_field_t;

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
    const uint8_t *body_start;
    const uint8_t *body_end;
    loop_field_t field;
    bool flash;
} loop_case_t;

static const loop_case_t cases[] = {
    {"esp.lp.setupi", xesploop_setupi_start, xesploop_setupi_end,
        NULL, NULL, LOOP_SETUP, false},
    {"esp.lp.setup", xesploop_setup_start, xesploop_setup_end,
        NULL, NULL, LOOP_SETUP, false},
    {"esp.lp.starti", xesploop_starti_start, xesploop_starti_end,
        xesploop_starti_body_start, xesploop_starti_body_end, LOOP_START, false},
    {"esp.lp.endi", xesploop_endi_start, xesploop_endi_end,
        xesploop_endi_body_start, xesploop_endi_body_end, LOOP_END, false},
    {"esp.lp.counti", xesploop_counti_start, xesploop_counti_end,
        xesploop_counti_body_start, xesploop_counti_body_end, LOOP_COUNT, false},
    {"esp.lp.count", xesploop_count_start, xesploop_count_end,
        xesploop_count_body_start, xesploop_count_body_end, LOOP_COUNT, false},
    {"esp.lp.setupi (flash)", xesploop_flash_setupi_start,
        xesploop_flash_setupi_end, NULL, NULL, LOOP_SETUP, true},
    {"esp.lp.endi (flash)", xesploop_flash_endi_start,
        xesploop_flash_endi_end, xesploop_flash_endi_body_start,
        xesploop_flash_endi_body_end, LOOP_END, true},
    {"esp.lp.counti (flash)", xesploop_flash_counti_start,
        xesploop_flash_counti_end, xesploop_flash_counti_body_start,
        xesploop_flash_counti_body_end, LOOP_COUNT, true},
};

typedef struct {
    const loop_case_t *test;
    const uint8_t *entry;
    uint32_t actual;
    probe_fault_t fault;
    bool executed;
} loop_call_t;

__attribute__((noinline)) static uint32_t guarded_loop_state(uint32_t unused0,
        uint32_t unused1, uint32_t unused2)
{
    (void)unused0;
    (void)unused1;
    (void)unused2;
    uint32_t old_state;
    uint32_t readback;
    __asm__ volatile("csrr %0, 0x7f1" : "=r"(old_state));
    uint32_t enabled = (old_state & ~3u) | 1u;
    __asm__ volatile("csrw 0x7f1, %0" :: "r"(enabled) : "memory");
    __asm__ volatile("csrr %0, 0x7f1" : "=r"(readback));
    __asm__ volatile("csrw 0x7f1, %0" :: "r"(old_state) : "memory");
    return readback;
}

static const char *loop_state_problem(void)
{
    uint32_t readback;
    probe_fault_t fault;
    portENTER_CRITICAL(&loop_mux);
    bool executed = probe_guarded_call(guarded_loop_state, 0, 0, 0,
            &readback, &fault);
    portEXIT_CRITICAL(&loop_mux);
    if (!executed)
    {
        ESP_LOGW(TAG, "HWLP state CSR trap cause=%lu pc=%08lx tval=%08lx",
                (unsigned long)fault.cause, (unsigned long)fault.pc,
                (unsigned long)fault.tval);
        return "HWLP state CSR";
    }
    ESP_LOGI(TAG, "HWLP state readback=%08lx", (unsigned long)readback);
    return (readback & 3u) != 0 ? NULL : "HWLP state disabled";
}

/* The outer guard covers CSR setup/restore; the inner guard covers the opcode. */
__attribute__((noinline)) static uint32_t guarded_loop_bridge(uint32_t address,
        uint32_t unused1, uint32_t unused2)
{
    (void)unused1;
    (void)unused2;
    loop_call_t *call = (loop_call_t *)(uintptr_t)address;
    const loop_case_t *test = call->test;
    uint32_t old_state;
    uint32_t old_loop[3];
    __asm__ volatile("csrr %0, 0x7f1" : "=r"(old_state));
    uint32_t enabled = (old_state & ~3u) | 1u;
    __asm__ volatile("csrw 0x7f1, %0" :: "r"(enabled) : "memory");
    if (old_state & 3u)
    {
        __asm__ volatile("csrr %0, 0x7c6" : "=r"(old_loop[0]));
        __asm__ volatile("csrr %0, 0x7c7" : "=r"(old_loop[1]));
        __asm__ volatile("csrr %0, 0x7c8" : "=r"(old_loop[2]));
    }
    if (test->field != LOOP_SETUP)
    {
        uint32_t start = (uint32_t)(uintptr_t)call->entry +
                (uintptr_t)test->body_start - (uintptr_t)test->start;
        uint32_t end = (uint32_t)(uintptr_t)call->entry +
                (uintptr_t)test->body_end - (uintptr_t)test->start;
        uint32_t initial_start = test->field == LOOP_START ? end : start;
        /* A stale end that matches the instruction after endi still loops
         * on P4 v3.1, so preload an end that cannot match. */
        uint32_t initial_end = test->field == LOOP_END ? 0 : end;
        uint32_t initial_count = test->field == LOOP_COUNT ? 1 : 3;
        __asm__ volatile("csrw 0x7c6, %0" :: "r"(initial_start) : "memory");
        __asm__ volatile("csrw 0x7c7, %0" :: "r"(initial_end) : "memory");
        __asm__ volatile("csrw 0x7c8, %0" :: "r"(initial_count) : "memory");
    }
    call->executed = probe_guarded_call(call->entry, 0, 3, 0,
            &call->actual, &call->fault);
    if (old_state & 3u)
    {
        __asm__ volatile("csrw 0x7c6, %0" :: "r"(old_loop[0]) : "memory");
        __asm__ volatile("csrw 0x7c7, %0" :: "r"(old_loop[1]) : "memory");
        __asm__ volatile("csrw 0x7c8, %0" :: "r"(old_loop[2]) : "memory");
    }
    __asm__ volatile("csrw 0x7f1, %0" :: "r"(old_state) : "memory");
    return 0;
}

static probe_status_t execute_case(uint8_t *code, const loop_case_t *test)
{
    size_t length = (size_t)(test->end - test->start);
    if (!length || length > PROBE_CODE_SIZE)
    {
        ESP_LOGE(TAG, "Invalid HWLP snippet: %s", test->name);
        return PROBE_FAIL;
    }

    if (!test->flash)
        memcpy(code, test->start, length);
    const uint8_t *entry = test->flash ? test->start : code;
    if (!test->flash && !probe_sync(code, PROBE_CODE_SIZE))
        return PROBE_FAIL;

    ESP_LOGI(TAG, "RUN %s (%u bytes)", test->name, (unsigned)length);
    loop_call_t call = {.test = test, .entry = entry, .executed = false};
    uint32_t ignored;
    probe_fault_t setup_fault;
    portENTER_CRITICAL(&loop_mux);
    bool setup_ok = probe_guarded_call(guarded_loop_bridge,
            (uint32_t)(uintptr_t)&call, 0, 0, &ignored, &setup_fault);
    portEXIT_CRITICAL(&loop_mux);

    if (!setup_ok)
    {
        ESP_LOGW(TAG, "%s: HWLP CSR trap cause=%lu pc=%08lx tval=%08lx",
                test->name, (unsigned long)setup_fault.cause,
                (unsigned long)setup_fault.pc, (unsigned long)setup_fault.tval);
        return PROBE_UNAVAILABLE;
    }
    if (!call.executed)
    {
        ESP_LOGW(TAG, "%s: trap cause=%lu pc=%08lx tval=%08lx", test->name,
                (unsigned long)call.fault.cause, (unsigned long)call.fault.pc,
                (unsigned long)call.fault.tval);
        return PROBE_UNAVAILABLE;
    }

    bool passed = call.actual == 9;
    ESP_LOGI(TAG, "%s: %s actual=%lu expected=9", test->name,
            passed ? "PASS" : "FAIL", (unsigned long)call.actual);
    return passed ? PROBE_PASS : PROBE_FAIL;
}

void probe_xesploop_run(uint8_t *code)
{
    const char *state_problem = loop_state_problem();
    if (state_problem)
    {
        probe_report("XESPLOOP", PROBE_UNAVAILABLE, 0, 0, state_problem);
        return;
    }

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
    char detail[64];
    snprintf(detail, sizeof(detail), "%u/%u passed, %u trapped",
            passed, (unsigned)PROBE_ARRAY_SIZE(cases), unavailable);
    probe_report("XESPLOOP", failed ? PROBE_FAIL :
            unavailable ? PROBE_UNAVAILABLE : PROBE_PASS,
            passed, PROBE_ARRAY_SIZE(cases), detail);
}
