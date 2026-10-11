/* Run the PIE instructions used by the ISA probe from flash and RAM.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "probe.h"

static const char *TAG = "simd_smoke";

extern void zcmp_characterize_run(void);
extern void zcmp_interrupt_run(void);
extern void benchmark_run(bool simd_valid);
extern void memory_benchmark_run(void);
extern void compressed_smoke_run(uint8_t *code);

#define CODE_SIZE PROBE_CODE_SIZE

typedef void (*simd_fn_t)(const uint8_t *left, const uint8_t *right,
        uint8_t *output);

#define SIMD_SNIPPET(name) \
    extern const uint8_t simd_##name##_start[]; \
    extern const uint8_t simd_##name##_end[]
SIMD_SNIPPET(copy);
SIMD_SNIPPET(xorq);
SIMD_SNIPPET(vaddu8);
SIMD_SNIPPET(andq);
SIMD_SNIPPET(orq);
SIMD_SNIPPET(vmaxu8);
SIMD_SNIPPET(vminu8);
#undef SIMD_SNIPPET

typedef enum {
    SIMD_COPY,
    SIMD_XOR,
    SIMD_ADD_SAT,
    SIMD_AND,
    SIMD_OR,
    SIMD_MAX,
    SIMD_MIN,
} simd_op_t;

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
    simd_op_t op;
} simd_case_t;

static const simd_case_t cases[] = {
    {"esp.vld.128.ip / esp.vst.128.ip", simd_copy_start, simd_copy_end, SIMD_COPY},
    {"esp.xorq", simd_xorq_start, simd_xorq_end, SIMD_XOR},
    {"esp.vadd.u8", simd_vaddu8_start, simd_vaddu8_end, SIMD_ADD_SAT},
    {"esp.andq", simd_andq_start, simd_andq_end, SIMD_AND},
    {"esp.orq", simd_orq_start, simd_orq_end, SIMD_OR},
    {"esp.vmax.u8", simd_vmaxu8_start, simd_vmaxu8_end, SIMD_MAX},
    {"esp.vmin.u8", simd_vminu8_start, simd_vminu8_end, SIMD_MIN},
};

static uint8_t expected_byte(simd_op_t op, uint8_t left, uint8_t right)
{
    switch (op)
    {
        case SIMD_COPY: return left;
        case SIMD_XOR: return left ^ right;
        case SIMD_ADD_SAT:
        {
            unsigned sum = (unsigned)left + right;
            return sum > 255 ? 255 : (uint8_t)sum;
        }
        case SIMD_AND: return left & right;
        case SIMD_OR: return left | right;
        case SIMD_MAX: return left > right ? left : right;
        case SIMD_MIN: return left < right ? left : right;
    }
    return 0;
}

static bool sync_code(void *code, size_t size)
{
    esp_err_t err = esp_cache_msync(code, size,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGE(TAG, "Data cache sync: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_cache_msync(code, size,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGE(TAG, "Instruction cache sync: %s", esp_err_to_name(err));
        return false;
    }
    __asm__ volatile("fence.i" ::: "memory");
    return true;
}

static bool run_case(const simd_case_t *test, simd_fn_t function,
        const char *location)
{
    uint8_t left[16] __attribute__((aligned(16)));
    uint8_t right[16] __attribute__((aligned(16)));
    uint8_t output[16] __attribute__((aligned(16))) = {0};
    uint8_t expected[16];
    for (unsigned i = 0; i < sizeof(left); i++)
    {
        left[i] = i == 0 ? 250 : (uint8_t)(i * 13);
        right[i] = i == 0 ? 10 : (uint8_t)(i * 7 + 1);
        expected[i] = expected_byte(test->op, left[i], right[i]);
    }

    ESP_LOGI(TAG, "RUN %s from %s", test->name, location);
    function(left, right, output);
    for (unsigned i = 0; i < sizeof(output); i++)
        if (output[i] != expected[i])
        {
            ESP_LOGE(TAG, "FAIL %s from %s byte %u: %02x expected %02x",
                    test->name, location, i, output[i], expected[i]);
            return false;
        }
    ESP_LOGI(TAG, "PASS %s from %s first=%02x last=%02x",
            test->name, location, output[0], output[15]);
    return true;
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "SIMD SMOKE START rev=%u.%u cores=%u",
            chip.revision / 100, chip.revision % 100, chip.cores);

    uint8_t *code = heap_caps_aligned_alloc(CODE_SIZE, CODE_SIZE,
            MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    if (!code)
    {
        ESP_LOGE(TAG, "Executable internal RAM allocation failed");
        return;
    }

    unsigned passed = 0;
    unsigned total = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        const simd_case_t *test = &cases[i];
        size_t length = (size_t)(test->end - test->start);
        if (!length || length > CODE_SIZE)
        {
            ESP_LOGE(TAG, "Invalid snippet length for %s: %u",
                    test->name, (unsigned)length);
            total += 2;
            continue;
        }

        total++;
        passed += run_case(test, (simd_fn_t)test->start, "flash");
        memcpy(code, test->start, length);
        total++;
        if (sync_code(code, CODE_SIZE))
            passed += run_case(test, (simd_fn_t)code, "RAM");
    }
    ESP_LOGI(TAG, "SIMD SMOKE COMPLETE %u/%u PASS", passed, total);

    probe_zca_run(code);
    compressed_smoke_run(code);
    probe_zb_run(code);
    probe_zcb_run(code);
    probe_zcmp_run(code);
    probe_zcmt_run(code);
    zcmp_characterize_run();
    benchmark_run(passed == total);
    memory_benchmark_run();
    zcmp_interrupt_run();
    heap_caps_free(code);
    ESP_LOGI(TAG, "EXTENSION PROBES COMPLETE");
}
