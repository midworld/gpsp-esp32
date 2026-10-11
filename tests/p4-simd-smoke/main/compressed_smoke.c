/* Check mixed-width execution and compare compressed and full-size ADDI.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "probe.h"

static const char *TAG = "compressed_smoke";
static portMUX_TYPE compressed_mux = portMUX_INITIALIZER_UNLOCKED;

extern const uint8_t compressed_mixed_start[];
extern const uint8_t compressed_mixed_word[];
extern const uint8_t compressed_mixed_end[];

#define BENCH_COUNT 100000U
#define BENCH_SAMPLES 9U

#define BENCH_SNIPPET(name) \
    extern const uint8_t compressed_bench_##name##_start[]; \
    extern const uint8_t compressed_bench_##name##_body[]; \
    extern const uint8_t compressed_bench_##name##_body_end[]; \
    extern const uint8_t compressed_bench_##name##_end[]
BENCH_SNIPPET(empty);
BENCH_SNIPPET(caddi);
BENCH_SNIPPET(addi);
#undef BENCH_SNIPPET

typedef uint32_t (*bench_fn_t)(unsigned count, uint32_t *result);

typedef struct {
    const uint8_t *start;
    const uint8_t *body;
    const uint8_t *body_end;
    const uint8_t *end;
    unsigned body_bytes;
} bench_case_t;

static const bench_case_t bench_cases[] = {
    {compressed_bench_empty_start, compressed_bench_empty_body,
            compressed_bench_empty_body_end, compressed_bench_empty_end, 0},
    {compressed_bench_caddi_start, compressed_bench_caddi_body,
            compressed_bench_caddi_body_end, compressed_bench_caddi_end, 32},
    {compressed_bench_addi_start, compressed_bench_addi_body,
            compressed_bench_addi_body_end, compressed_bench_addi_end, 64},
};

static void benchmark_addi_width(uint8_t *code)
{
    if (((uintptr_t)bench_cases[1].body & 63) !=
            ((uintptr_t)bench_cases[2].body & 63))
    {
        ESP_LOGE(TAG, "RV32C benchmark loop alignment differs");
        return;
    }

    uint8_t *extra[2] = {
        heap_caps_aligned_alloc(PROBE_CODE_ALIGNMENT, PROBE_CODE_SIZE,
                MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL),
        heap_caps_aligned_alloc(PROBE_CODE_ALIGNMENT, PROBE_CODE_SIZE,
                MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL)
    };
    if (!extra[0] || !extra[1])
    {
        ESP_LOGE(TAG, "RV32C benchmark RAM allocation failed");
        goto done;
    }

    for (unsigned location = 0; location < 2; location++)
    {
        bench_fn_t functions[3];
        uint32_t best[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
        for (unsigned index = 0; index < 3; index++)
        {
            const bench_case_t *test = &bench_cases[index];
            size_t length = (size_t)(test->end - test->start);
            size_t body_bytes = (size_t)(test->body_end - test->body);
            if (!length || length > PROBE_CODE_SIZE ||
                    ((uintptr_t)test->start & 3) != 0 ||
                    (size_t)(test->body - test->start) != 8 ||
                    body_bytes != test->body_bytes)
            {
                ESP_LOGE(TAG, "RV32C benchmark instruction layout failed");
                goto done;
            }
            const uint8_t *entry = test->start;
            if (location)
            {
                uint8_t *destination = index == 0 ? code : extra[index - 1];
                memcpy(destination, entry, length);
                if (!probe_sync(destination, PROBE_CODE_SIZE))
                    goto done;
                entry = destination;
            }
            functions[index] = (bench_fn_t)entry;
            uint32_t result = 0;
            functions[index](100, &result);
            if (result != (index ? 1600U : 0U))
            {
                ESP_LOGE(TAG, "RV32C benchmark warmup result mismatch");
                goto done;
            }
        }

        for (unsigned sample = 0; sample < BENCH_SAMPLES; sample++)
            for (unsigned slot = 0; slot < 3; slot++)
            {
                unsigned index = (sample & 1) ? 2 - slot : slot;
                uint32_t result = 0;
                uint32_t cycles = functions[index](BENCH_COUNT, &result);
                if (result != (index ? 16U * BENCH_COUNT : 0U))
                {
                    ESP_LOGE(TAG, "RV32C benchmark result mismatch");
                    goto done;
                }
                if (cycles < best[index])
                    best[index] = cycles;
            }

        uint32_t empty = (uint32_t)((uint64_t)best[0] * 100 / BENCH_COUNT);
        uint32_t compressed = (uint32_t)((uint64_t)best[1] * 100 / BENCH_COUNT);
        uint32_t full = (uint32_t)((uint64_t)best[2] * 100 / BENCH_COUNT);
        ESP_LOGI(TAG, "16 adds %s: empty=%lu.%02lu c.addi=%lu.%02lu "
                "addi=%lu.%02lu cycles/loop; body=32/64 bytes",
                location ? "RAM" : "flash",
                (unsigned long)(empty / 100), (unsigned long)(empty % 100),
                (unsigned long)(compressed / 100), (unsigned long)(compressed % 100),
                (unsigned long)(full / 100), (unsigned long)(full % 100));
    }

done:
    heap_caps_free(extra[0]);
    heap_caps_free(extra[1]);
}

static bool run_mixed(const void *entry, const char *location)
{
    uint32_t result = 0;
    probe_fault_t fault;
    portENTER_CRITICAL(&compressed_mux);
    bool executed = probe_guarded_call(entry, 0, 0, 0, &result, &fault);
    portEXIT_CRITICAL(&compressed_mux);
    if (!executed)
    {
        ESP_LOGE(TAG, "%s trap cause=%lu pc=%08lx tval=%08lx", location,
                (unsigned long)fault.cause, (unsigned long)fault.pc,
                (unsigned long)fault.tval);
        return false;
    }
    ESP_LOGI(TAG, "%s: %s result=%lu", location,
            result == 42 ? "PASS" : "FAIL", (unsigned long)result);
    return result == 42;
}

void compressed_smoke_run(uint8_t *code)
{
    size_t length = (size_t)(compressed_mixed_end - compressed_mixed_start);
    size_t word_offset = (size_t)(compressed_mixed_word - compressed_mixed_start);
    const uint8_t *bytes = compressed_mixed_start;
    if (length != 10 || word_offset != 2 ||
            ((uintptr_t)bytes & 3) != 0 ||
            (bytes[0] & 3) == 3 || (bytes[2] & 3) != 3 ||
            (bytes[6] & 3) == 3 || (bytes[8] & 3) == 3)
    {
        ESP_LOGE(TAG, "Unexpected instruction layout: length=%u word_offset=%u",
                (unsigned)length, (unsigned)word_offset);
        probe_report("RV32C MIXED WIDTH", PROBE_FAIL, 0, 2, NULL);
        return;
    }

    unsigned passed = run_mixed(bytes, "flash");
    memcpy(code, bytes, length);
    if (probe_sync(code, PROBE_CODE_SIZE))
        passed += run_mixed(code, "RAM");
    probe_report("RV32C MIXED WIDTH", passed == 2 ? PROBE_PASS : PROBE_FAIL,
            passed, 2, NULL);
    benchmark_addi_width(code);
}
