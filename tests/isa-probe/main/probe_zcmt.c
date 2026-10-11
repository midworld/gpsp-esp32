/* ISA instruction probes.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "probe.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define JVT_ALIGNMENT 64
#define JVT_ENTRIES 256

static const char *TAG = "isa_probe";

#define ZCMT_SNIPPET(name) \
    extern const uint8_t zcmt_##name##_start[]; \
    extern const uint8_t zcmt_##name##_end[]; \
    extern const uint8_t zcmt_##name##_target[]
ZCMT_SNIPPET(jt0);
ZCMT_SNIPPET(jt31);
ZCMT_SNIPPET(jalt32);
ZCMT_SNIPPET(jalt255);
extern const uint8_t zcmt_flash_jt0_start[];
extern const uint8_t zcmt_flash_jt0_target[];
extern const uint8_t zcmt_flash_jt0_end[];
#undef ZCMT_SNIPPET
extern const uint8_t zcmt_jalt32_resume[];
extern const uint8_t zcmt_jalt255_resume[];

typedef struct {
    const char *name;
    unsigned index;
    const uint8_t *start;
    const uint8_t *end;
    const uint8_t *target;
    const uint8_t *resume;
    uint32_t expected;
    bool flash;
} zcmt_case_t;

static const zcmt_case_t zcmt_cases[] = {
    {"cm.jt 0", 0, zcmt_jt0_start, zcmt_jt0_end,
        zcmt_jt0_target, NULL, 0x10203040, false},
    {"cm.jt 31", 31, zcmt_jt31_start, zcmt_jt31_end,
        zcmt_jt31_target, NULL, 0x11223344, false},
    {"cm.jalt 32", 32, zcmt_jalt32_start, zcmt_jalt32_end,
        zcmt_jalt32_target, zcmt_jalt32_resume, 0x55667788, false},
    {"cm.jalt 255", 255, zcmt_jalt255_start, zcmt_jalt255_end,
        zcmt_jalt255_target, zcmt_jalt255_resume, 0x99aabbcc, false},
};

static const zcmt_case_t flash_case = {
    "cm.jt 0 (flash)", 0, zcmt_flash_jt0_start, zcmt_flash_jt0_end,
    zcmt_flash_jt0_target, NULL, 0x10203040, true,
};

static portMUX_TYPE zcmt_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    const uint8_t *entry;
    uint32_t *table;
    uint32_t *observed_link;
    uint32_t *saved_ra;
    uint32_t readback;
    uint32_t returned;
    probe_fault_t fault;
    bool executed;
} zcmt_call_t;

/* Guard CSR setup as well as the jump, then restore jvt after a jump trap. */
__attribute__((noinline)) static uint32_t guarded_zcmt_bridge(uint32_t address,
        uint32_t unused1, uint32_t unused2)
{
    (void)unused1;
    (void)unused2;
    zcmt_call_t *call = (zcmt_call_t *)(uintptr_t)address;
    uint32_t old_jvt;
    __asm__ volatile("csrrw %0, 0x017, %1" : "=r"(old_jvt) :
            "r"(call->table) : "memory");
    __asm__ volatile("csrr %0, 0x017" : "=r"(call->readback) :: "memory");
    call->executed = probe_guarded_call_access(call->entry, 0x12345678,
            (uint32_t)(uintptr_t)call->observed_link,
            (uint32_t)(uintptr_t)call->saved_ra,
            &call->returned, &call->fault);
    __asm__ volatile("csrw 0x017, %0" :: "r"(old_jvt) : "memory");
    return 0;
}

static probe_status_t execute_zcmt_case(uint8_t *code, uint32_t *table,
        const zcmt_case_t *test, bool *jvt_matches)
{
    size_t length = (size_t)(test->end - test->start);
    if (!length || length > PROBE_CODE_SIZE || test->index >= JVT_ENTRIES)
    {
        ESP_LOGE(TAG, "Invalid Zcmt snippet: %s", test->name);
        return PROBE_FAIL;
    }

    if (!test->flash)
        memcpy(code, test->start, length);
    const uint8_t *entry = test->flash ? test->start : code;
    for (unsigned i = 0; i < JVT_ENTRIES; i++)
        table[i] = 0;
    table[test->index] = (uint32_t)(uintptr_t)
            (entry + ((uintptr_t)test->target - (uintptr_t)test->start));
    if ((!test->flash && !probe_sync(code, PROBE_CODE_SIZE)) ||
            !probe_sync(table, JVT_ENTRIES * sizeof(*table)))
        return PROBE_FAIL;

    uint32_t observed_link = 0xdeadbeef;
    uint32_t saved_ra = 0;
    ESP_LOGI(TAG, "RUN %s table=%08lx target=%08lx", test->name,
            (unsigned long)(uintptr_t)table,
            (unsigned long)table[test->index]);

    zcmt_call_t call = {
        .entry = entry,
        .table = table,
        .observed_link = &observed_link,
        .saved_ra = &saved_ra,
        .readback = 0,
        .executed = false,
    };
    uint32_t ignored;
    probe_fault_t setup_fault;
    /* jvt is per core; keep the task on this hart through setup and restore. */
    portENTER_CRITICAL(&zcmt_mux);
    bool setup_ok = probe_guarded_call(guarded_zcmt_bridge,
            (uint32_t)(uintptr_t)&call, 0, 0, &ignored, &setup_fault);
    portEXIT_CRITICAL(&zcmt_mux);

    if (!setup_ok)
    {
        ESP_LOGW(TAG, "%s: jvt CSR trap cause=%lu pc=%08lx tval=%08lx",
                test->name, (unsigned long)setup_fault.cause,
                (unsigned long)setup_fault.pc, (unsigned long)setup_fault.tval);
        return PROBE_UNAVAILABLE;
    }
    if (jvt_matches)
        *jvt_matches = call.readback == (uint32_t)(uintptr_t)table;
    /* A broken CSR readback does not prove that the write was ignored. */

    if (!call.executed)
    {
        ESP_LOGW(TAG, "%s: trap cause=%lu pc=%08lx tval=%08lx jvt=%08lx",
                test->name, (unsigned long)call.fault.cause,
                (unsigned long)call.fault.pc, (unsigned long)call.fault.tval,
                (unsigned long)call.readback);
        return PROBE_UNAVAILABLE;
    }

    uint32_t expected_link = test->resume ? (uint32_t)(uintptr_t)
            (entry + ((uintptr_t)test->resume - (uintptr_t)test->start)) : 0xdeadbeef;
    bool passed = call.returned == test->expected &&
            observed_link == expected_link;
    ESP_LOGI(TAG, "%s: %s returned=%08lx expected=%08lx link=%08lx expected=%08lx jvt=%08lx",
            test->name, passed ? "PASS" : "FAIL", (unsigned long)call.returned,
            (unsigned long)test->expected, (unsigned long)observed_link,
            (unsigned long)expected_link, (unsigned long)call.readback);
    return passed ? PROBE_PASS : PROBE_FAIL;
}

void probe_zcmt_run(uint8_t *code)
{
    uint32_t *table = heap_caps_aligned_alloc(JVT_ALIGNMENT,
            JVT_ENTRIES * sizeof(*table), MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    if (!table)
    {
        probe_report("ZCMT", PROBE_UNAVAILABLE, 0, 0, "table allocation");
        return;
    }

    unsigned passed = 0;
    unsigned unavailable = 0;
    unsigned failed = 0;
    unsigned blocked = 0;
    bool jvt_matches = false;
    for (unsigned i = 0; i < PROBE_ARRAY_SIZE(zcmt_cases); i++)
    {
        probe_status_t result = execute_zcmt_case(code, table,
                &zcmt_cases[i], i == 0 ? &jvt_matches : NULL);
        passed += result == PROBE_PASS;
        unavailable += result == PROBE_UNAVAILABLE;
        failed += result == PROBE_FAIL;
        /* A trapped first jump plus zero readback leaves the shared table unverified. */
        if (i == 0 && result != PROBE_PASS && !jvt_matches)
        {
            blocked = PROBE_ARRAY_SIZE(zcmt_cases) - 1;
            for (unsigned j = 1; j < PROBE_ARRAY_SIZE(zcmt_cases); j++)
                ESP_LOGW(TAG, "%s: BLOCKED (jvt readback mismatch; cm.jt 0 failed)",
                        zcmt_cases[j].name);
            break;
        }
    }
    probe_status_t flash_result = execute_zcmt_case(code, table,
            &flash_case, NULL);
    ESP_LOGI(TAG, "ZCMT flash comparison: %s",
            flash_result == PROBE_PASS ? "PASS" :
            flash_result == PROBE_UNAVAILABLE ? "TRAPPED" : "FAIL");
    char detail[64];
    snprintf(detail, sizeof(detail), "%u/%u passed, %u trapped, %u blocked",
            passed, (unsigned)PROBE_ARRAY_SIZE(zcmt_cases), unavailable, blocked);
    probe_report("ZCMT", failed ? PROBE_FAIL :
            unavailable ? PROBE_UNAVAILABLE : PROBE_PASS,
            passed, PROBE_ARRAY_SIZE(zcmt_cases), detail);
    heap_caps_free(table);
}
