/* Compare equivalent save/restore pairs and 16-byte data operations.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BENCH_ITERATIONS 100000U
#define BENCH_SAMPLES 9U

static const char *TAG = "isa_bench";

extern uint32_t benchmark_empty(unsigned count);
extern uint32_t benchmark_manual_pair(unsigned count);
extern uint32_t benchmark_zcmp_pair(unsigned count);

typedef uint32_t (*data_bench_t)(const void *left, const void *right,
        void *output, unsigned count);

#define SIMD_BENCH(name) \
    extern uint32_t benchmark_simd_##name(const void *, const void *, \
            void *, unsigned)
SIMD_BENCH(copy);
SIMD_BENCH(xorq);
SIMD_BENCH(vaddu8);
SIMD_BENCH(andq);
SIMD_BENCH(orq);
SIMD_BENCH(vmaxu8);
SIMD_BENCH(vminu8);
#undef SIMD_BENCH

static inline uint32_t cycle_count(void)
{
    uint32_t cycles;
    __asm__ volatile("rdcycle %0" : "=r"(cycles) :: "memory");
    return cycles;
}

#define SCALAR_WORD(name, value) \
    static uint32_t IRAM_ATTR __attribute__((noinline)) \
    benchmark_scalar_##name(const void *left_data, const void *right_data, \
            void *output_data, unsigned count) \
    { \
        const volatile uint32_t *left = left_data; \
        const volatile uint32_t *right = right_data; \
        volatile uint32_t *output = output_data; \
        uint32_t start = cycle_count(); \
        for (unsigned i = 0; i < count; i++) \
            for (unsigned word = 0; word < 4; word++) \
            { \
                uint32_t a = left[word]; \
                uint32_t b = right[word]; \
                output[word] = (value); \
            } \
        return cycle_count() - start; \
    }
SCALAR_WORD(xorq, a ^ b)
SCALAR_WORD(andq, a & b)
SCALAR_WORD(orq, a | b)
#undef SCALAR_WORD

static uint32_t IRAM_ATTR __attribute__((noinline))
benchmark_scalar_copy(const void *left_data, const void *right_data,
        void *output_data, unsigned count)
{
    (void)right_data;
    const volatile uint32_t *left = left_data;
    volatile uint32_t *output = output_data;
    uint32_t start = cycle_count();
    for (unsigned i = 0; i < count; i++)
        for (unsigned word = 0; word < 4; word++)
            output[word] = left[word];
    return cycle_count() - start;
}

#define SCALAR_BYTE(name, value) \
    static uint32_t IRAM_ATTR __attribute__((noinline)) \
    benchmark_scalar_##name(const void *left_data, const void *right_data, \
            void *output_data, unsigned count) \
    { \
        const volatile uint8_t *left = left_data; \
        const volatile uint8_t *right = right_data; \
        volatile uint8_t *output = output_data; \
        uint32_t start = cycle_count(); \
        for (unsigned i = 0; i < count; i++) \
            for (unsigned byte = 0; byte < 16; byte++) \
            { \
                unsigned a = left[byte]; \
                unsigned b = right[byte]; \
                output[byte] = (uint8_t)(value); \
            } \
        return cycle_count() - start; \
    }
SCALAR_BYTE(vaddu8, a + b > 255 ? 255 : a + b)
SCALAR_BYTE(vmaxu8, a > b ? a : b)
SCALAR_BYTE(vminu8, a < b ? a : b)
#undef SCALAR_BYTE

typedef struct {
    const char *name;
    data_bench_t scalar;
    data_bench_t simd;
} data_case_t;

static const data_case_t cases[] = {
    {"copy", benchmark_scalar_copy, benchmark_simd_copy},
    {"xorq", benchmark_scalar_xorq, benchmark_simd_xorq},
    {"vadd.u8", benchmark_scalar_vaddu8, benchmark_simd_vaddu8},
    {"andq", benchmark_scalar_andq, benchmark_simd_andq},
    {"orq", benchmark_scalar_orq, benchmark_simd_orq},
    {"vmax.u8", benchmark_scalar_vmaxu8, benchmark_simd_vmaxu8},
    {"vmin.u8", benchmark_scalar_vminu8, benchmark_simd_vminu8},
};

static uint32_t smaller(uint32_t left, uint32_t right)
{
    return left < right ? left : right;
}

static void log_pair(const char *name, const char *first_name,
        const char *second_name, uint32_t first, uint32_t second)
{
    uint32_t first_100 = (uint32_t)((uint64_t)first * 100 / BENCH_ITERATIONS);
    uint32_t second_100 = (uint32_t)((uint64_t)second * 100 / BENCH_ITERATIONS);
    uint32_t ratio_100 = second ?
            (uint32_t)((uint64_t)first * 100 / second) : 0;
    ESP_LOGI(TAG, "%s %s=%lu.%02lu %s=%lu.%02lu cyc/op %s/%s=%lu.%02lux",
            name, first_name, (unsigned long)(first_100 / 100),
            (unsigned long)(first_100 % 100),
            second_name,
            (unsigned long)(second_100 / 100),
            (unsigned long)(second_100 % 100),
            second_name, first_name,
            (unsigned long)(ratio_100 / 100),
            (unsigned long)(ratio_100 % 100));
}

static void benchmark_stack(void)
{
    uint32_t empty = UINT32_MAX;
    uint32_t manual = UINT32_MAX;
    uint32_t zcmp = UINT32_MAX;
    benchmark_empty(100);
    benchmark_manual_pair(100);
    benchmark_zcmp_pair(100);
    for (unsigned sample = 0; sample < BENCH_SAMPLES; sample++)
    {
        empty = smaller(empty, benchmark_empty(BENCH_ITERATIONS));
        if (sample & 1)
        {
            zcmp = smaller(zcmp, benchmark_zcmp_pair(BENCH_ITERATIONS));
            manual = smaller(manual, benchmark_manual_pair(BENCH_ITERATIONS));
        }
        else
        {
            manual = smaller(manual, benchmark_manual_pair(BENCH_ITERATIONS));
            zcmp = smaller(zcmp, benchmark_zcmp_pair(BENCH_ITERATIONS));
        }
    }
    if (manual <= empty || zcmp <= empty)
    {
        ESP_LOGE(TAG, "Zcmp timing failed");
        return;
    }
    log_pair("{ra,s0-s1} save+restore", "manual", "Zcmp",
            manual - empty, zcmp - empty);
}

static void benchmark_data(void)
{
    uint32_t left[4] __attribute__((aligned(16)));
    uint32_t right[4] __attribute__((aligned(16)));
    uint32_t scalar_output[4] __attribute__((aligned(16))) = {0};
    uint32_t simd_output[4] __attribute__((aligned(16))) = {0};
    uint8_t *left_bytes = (uint8_t *)left;
    uint8_t *right_bytes = (uint8_t *)right;
    for (unsigned byte = 0; byte < 16; byte++)
    {
        left_bytes[byte] = byte == 0 ? 250 : (uint8_t)(byte * 13);
        right_bytes[byte] = byte == 0 ? 10 : (uint8_t)(byte * 7 + 1);
    }

    for (unsigned index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        const data_case_t *test = &cases[index];
        test->scalar(left, right, scalar_output, 1);
        test->simd(left, right, simd_output, 1);
        if (memcmp(scalar_output, simd_output, sizeof(simd_output)))
        {
            ESP_LOGE(TAG, "%s result mismatch; timing skipped", test->name);
            continue;
        }
        test->scalar(left, right, scalar_output, 100);
        test->simd(left, right, simd_output, 100);

        uint32_t scalar = UINT32_MAX;
        uint32_t simd = UINT32_MAX;
        for (unsigned sample = 0; sample < BENCH_SAMPLES; sample++)
        {
            if (sample & 1)
            {
                simd = smaller(simd, test->simd(left, right, simd_output,
                        BENCH_ITERATIONS));
                scalar = smaller(scalar, test->scalar(left, right, scalar_output,
                        BENCH_ITERATIONS));
            }
            else
            {
                scalar = smaller(scalar, test->scalar(left, right, scalar_output,
                        BENCH_ITERATIONS));
                simd = smaller(simd, test->simd(left, right, simd_output,
                        BENCH_ITERATIONS));
            }
        }
        if (memcmp(scalar_output, simd_output, sizeof(simd_output)))
        {
            ESP_LOGE(TAG, "%s result changed during timing", test->name);
            continue;
        }
        log_pair(test->name, "scalar", "SIMD", scalar, simd);
    }
}

void benchmark_run(bool simd_valid)
{
    if (xPortGetCoreID() != 0)
    {
        ESP_LOGE(TAG, "Benchmark requires core0");
        return;
    }
    ESP_LOGI(TAG, "%u operations, best of %u, internal RAM, scalar C -O2",
            BENCH_ITERATIONS, BENCH_SAMPLES);
    benchmark_stack();
    if (simd_valid)
    {
        ESP_LOGI(TAG, "SIMD kernels include loads/stores on hot 16-byte buffers");
        benchmark_data();
    }
    else
        ESP_LOGW(TAG, "SIMD timing skipped after smoke-test failure");
}
