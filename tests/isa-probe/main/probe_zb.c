/* RV32 Zba, Zbb, and Zbs instruction probes.
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
static portMUX_TYPE zb_mux = portMUX_INITIALIZER_UNLOCKED;

typedef enum {
    ZBA,
    ZBB,
    ZBS,
} zb_group_t;

#define ZB_SNIPPET(name) \
    extern const uint8_t zb_##name##_start[]; \
    extern const uint8_t zb_##name##_end[]
ZB_SNIPPET(sh1add);
ZB_SNIPPET(sh2add);
ZB_SNIPPET(sh3add);
ZB_SNIPPET(andn);
ZB_SNIPPET(orn);
ZB_SNIPPET(xnor);
ZB_SNIPPET(clz);
ZB_SNIPPET(ctz);
ZB_SNIPPET(cpop);
ZB_SNIPPET(max);
ZB_SNIPPET(maxu);
ZB_SNIPPET(min);
ZB_SNIPPET(minu);
ZB_SNIPPET(orcb);
ZB_SNIPPET(rev8);
ZB_SNIPPET(rol);
ZB_SNIPPET(ror);
ZB_SNIPPET(rori31);
ZB_SNIPPET(sextb);
ZB_SNIPPET(sexth);
ZB_SNIPPET(zexth);
ZB_SNIPPET(bclr);
ZB_SNIPPET(bclri0);
ZB_SNIPPET(bext);
ZB_SNIPPET(bexti0);
ZB_SNIPPET(binv);
ZB_SNIPPET(binvi0);
ZB_SNIPPET(bset);
ZB_SNIPPET(bseti0);
#undef ZB_SNIPPET

typedef struct {
    zb_group_t group;
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
    uint32_t input;
    uint32_t arg;
    uint32_t expected;
} zb_case_t;

#define CASE(group, name, input, arg, expected) \
    {group, #name, zb_##name##_start, zb_##name##_end, input, arg, expected}

static const zb_case_t cases[] = {
    CASE(ZBA, sh1add, 0x40000001, 3, 0x80000005),
    CASE(ZBA, sh2add, 0x40000001, 3, 7),
    CASE(ZBA, sh3add, 0x20000000, 1, 1),

    CASE(ZBB, andn, 0xf0f0, 0x00ff, 0xf000),
    CASE(ZBB, orn, 0x0f00, 0x00f0, 0xffffff0f),
    CASE(ZBB, xnor, 0xaaaaaaaa, 0x55555555, 0),
    CASE(ZBB, clz, 0x00008000, 0, 16),
    CASE(ZBB, clz, 0, 0, 32),
    CASE(ZBB, ctz, 0x80000000, 0, 31),
    CASE(ZBB, ctz, 0, 0, 32),
    CASE(ZBB, cpop, 0xf0f00001, 0, 9),
    CASE(ZBB, cpop, 0, 0, 0),
    CASE(ZBB, max, 0x80000000, 1, 1),
    CASE(ZBB, maxu, 0x80000000, 1, 0x80000000),
    CASE(ZBB, min, 0x80000000, 1, 0x80000000),
    CASE(ZBB, minu, 0x80000000, 1, 1),
    CASE(ZBB, orcb, 0x00120080, 0, 0x00ff00ff),
    CASE(ZBB, rev8, 0x12345678, 0, 0x78563412),
    CASE(ZBB, rol, 0x80000001, 1, 3),
    CASE(ZBB, rol, 0x12345678, 32, 0x12345678),
    CASE(ZBB, ror, 0x80000001, 1, 0xc0000000),
    CASE(ZBB, ror, 0x12345678, 0, 0x12345678),
    CASE(ZBB, rori31, 0x80000001, 0, 3),
    CASE(ZBB, sextb, 0x80, 0, 0xffffff80),
    CASE(ZBB, sextb, 0x7f, 0, 0x7f),
    CASE(ZBB, sexth, 0x8000, 0, 0xffff8000),
    CASE(ZBB, sexth, 0x7fff, 0, 0x7fff),
    CASE(ZBB, zexth, 0xffff8000, 0, 0x8000),

    CASE(ZBS, bclr, 0xffffffff, 31, 0x7fffffff),
    CASE(ZBS, bclr, 1, 32, 0),
    CASE(ZBS, bclri0, 0xffffffff, 0, 0xfffffffe),
    CASE(ZBS, bext, 0x80000000, 31, 1),
    CASE(ZBS, bext, 1, 32, 1),
    CASE(ZBS, bexti0, 1, 0, 1),
    CASE(ZBS, binv, 0, 31, 0x80000000),
    CASE(ZBS, binv, 0, 32, 1),
    CASE(ZBS, binvi0, 0xffffffff, 0, 0xfffffffe),
    CASE(ZBS, bset, 0, 31, 0x80000000),
    CASE(ZBS, bset, 0, 32, 1),
    CASE(ZBS, bseti0, 0, 0, 1),
};

#undef CASE

static probe_status_t execute_case(uint8_t *code, const zb_case_t *test)
{
    size_t length = (size_t)(test->end - test->start);
    if (!length || length > PROBE_CODE_SIZE)
    {
        ESP_LOGE(TAG, "Invalid Zb snippet: %s", test->name);
        return PROBE_FAIL;
    }

    memcpy(code, test->start, length);
    if (!probe_sync(code, PROBE_CODE_SIZE))
        return PROBE_FAIL;

    ESP_LOGI(TAG, "RUN %s (%u bytes)", test->name, (unsigned)length);
    uint32_t actual;
    probe_fault_t fault;
    portENTER_CRITICAL(&zb_mux);
    bool executed = probe_guarded_call(code, test->input, test->arg, 0,
            &actual, &fault);
    portEXIT_CRITICAL(&zb_mux);
    if (!executed)
    {
        ESP_LOGW(TAG, "%s: trap cause=%lu pc=%08lx tval=%08lx", test->name,
                (unsigned long)fault.cause, (unsigned long)fault.pc,
                (unsigned long)fault.tval);
        return PROBE_UNAVAILABLE;
    }
    bool passed = actual == test->expected;
    ESP_LOGI(TAG, "%s: %s actual=%08lx expected=%08lx", test->name,
            passed ? "PASS" : "FAIL", (unsigned long)actual,
            (unsigned long)test->expected);
    return passed ? PROBE_PASS : PROBE_FAIL;
}

void probe_zb_run(uint8_t *code)
{
    static const char *const group_names[] = {"ZBA", "ZBB", "ZBS"};
    for (unsigned group = ZBA; group <= ZBS; group++)
    {
        unsigned passed = 0;
        unsigned unavailable = 0;
        unsigned failed = 0;
        unsigned total = 0;
        for (unsigned i = 0; i < PROBE_ARRAY_SIZE(cases); i++)
        {
            if (cases[i].group != group)
                continue;
            total++;
            probe_status_t result = execute_case(code, &cases[i]);
            passed += result == PROBE_PASS;
            unavailable += result == PROBE_UNAVAILABLE;
            failed += result == PROBE_FAIL;
        }
        if (unavailable)
        {
            char detail[64];
            snprintf(detail, sizeof(detail), "%u/%u passed, %u illegal",
                    passed, total, unavailable);
            probe_report(group_names[group], failed ? PROBE_FAIL : PROBE_UNAVAILABLE,
                    passed, total, detail);
        }
        else
            probe_report(group_names[group], failed ? PROBE_FAIL : PROBE_PASS,
                    passed, total, NULL);
    }
}
