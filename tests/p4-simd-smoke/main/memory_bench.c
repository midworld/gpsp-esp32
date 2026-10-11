/* Measure hot and streaming CPU access to internal RAM and PSRAM.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define KIB 1024U
#define MIB (1024U * KIB)
#define TOTAL_BYTES (4U * MIB)
#define SAMPLES 5U

static const char *TAG = "memory_bench";
static volatile uint32_t read_sink;
static void *(*volatile copy_fn)(void *, const void *, size_t) = memcpy;
static void *(*volatile set_fn)(void *, int, size_t) = memset;

typedef struct {
    const char *name;
    uint8_t *source;
    uint8_t *target;
    size_t capacity;
    bool evict_read;
} memory_pair_t;

typedef enum {
    READ,
    WRITE,
    COPY,
} operation_t;

static uint32_t IRAM_ATTR __attribute__((noinline))
read_words(const volatile uint32_t *source, size_t words, unsigned passes)
{
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
    uint32_t d = 0;
    for (unsigned pass = 0; pass < passes; pass++)
        for (size_t word = 0; word < words; word += 4)
        {
            a += source[word];
            b += source[word + 1];
            c += source[word + 2];
            d += source[word + 3];
        }
    return a + b + c + d;
}

static uint32_t run_operation(const memory_pair_t *pair, size_t bytes,
        unsigned passes, operation_t operation)
{
    switch (operation)
    {
        case READ:
            return read_words((const volatile uint32_t *)pair->source,
                    bytes / sizeof(uint32_t), passes);
        case WRITE:
            for (unsigned pass = 0; pass < passes; pass++)
                set_fn(pair->target, 0xa5, bytes);
            return 0;
        case COPY:
            for (unsigned pass = 0; pass < passes; pass++)
                copy_fn(pair->target, pair->source, bytes);
            return 0;
    }
    return 0;
}

static bool verify_operation(const memory_pair_t *pair, size_t bytes,
        unsigned passes, operation_t operation, uint32_t result,
        uint32_t expected)
{
    if (operation == READ)
        return result == expected * passes;
    if (operation == COPY)
        return memcmp(pair->target, pair->source, bytes) == 0;
    for (size_t byte = 0; byte < bytes; byte++)
        if (pair->target[byte] != 0xa5)
            return false;
    return true;
}

static void run_case(const memory_pair_t *pair, size_t bytes,
        operation_t operation)
{
    static const char *const names[] = {"read32", "memset", "memcpy"};
    unsigned passes = (TOTAL_BYTES + bytes - 1) / bytes;
    uint32_t expected = read_words((const volatile uint32_t *)pair->source,
            bytes / sizeof(uint32_t), 1);
    read_sink = expected;
    run_operation(pair, bytes, 1, operation);

    int64_t best_us = INT64_MAX;
    for (unsigned sample = 0; sample < SAMPLES; sample++)
    {
        int64_t start = esp_timer_get_time();
        uint32_t result = run_operation(pair, bytes, passes, operation);
        int64_t elapsed = esp_timer_get_time() - start;
        read_sink = result;
        if (!verify_operation(pair, bytes, passes, operation, result, expected))
        {
            ESP_LOGE(TAG, "%s %s %u KiB: data mismatch", pair->name,
                    names[operation], (unsigned)(bytes / KIB));
            return;
        }
        if (elapsed < best_us)
            best_us = elapsed;
    }

    if (best_us <= 0)
    {
        ESP_LOGE(TAG, "%s %s %u KiB: timer failed", pair->name,
                names[operation], (unsigned)(bytes / KIB));
        return;
    }
    uint64_t mib_per_sec_100 =
            (uint64_t)bytes * passes * 100000000ULL / (uint64_t)best_us / MIB;
    ESP_LOGI(TAG, "%s %-6s %4u KiB: %lu.%02lu MiB/s (%lld us, %u passes)",
            pair->name, names[operation], (unsigned)(bytes / KIB),
            (unsigned long)(mib_per_sec_100 / 100),
            (unsigned long)(mib_per_sec_100 % 100),
            (long long)best_us, passes);
}

static void run_evicted_read(const memory_pair_t *pair, size_t bytes)
{
    uint32_t expected = read_words((const volatile uint32_t *)pair->source,
            bytes / sizeof(uint32_t), 1);
    int64_t best_us = INT64_MAX;
    for (unsigned sample = 0; sample < SAMPLES; sample++)
    {
        /* Read a separate region larger than L2 before the timed access. */
        read_sink = read_words((const volatile uint32_t *)pair->target,
                pair->capacity / sizeof(uint32_t), 1);
        int64_t start = esp_timer_get_time();
        uint32_t result = read_words((const volatile uint32_t *)pair->source,
                bytes / sizeof(uint32_t), 1);
        int64_t elapsed = esp_timer_get_time() - start;
        read_sink = result;
        if (result != expected)
        {
            ESP_LOGE(TAG, "%s cold32 %u KiB: data mismatch", pair->name,
                    (unsigned)(bytes / KIB));
            return;
        }
        if (elapsed < best_us)
            best_us = elapsed;
    }
    if (best_us <= 0)
    {
        ESP_LOGE(TAG, "%s cold32 %u KiB: timer failed", pair->name,
                (unsigned)(bytes / KIB));
        return;
    }
    uint64_t mib_per_sec_100 =
            (uint64_t)bytes * 100000000ULL / (uint64_t)best_us / MIB;
    ESP_LOGI(TAG, "%s cold32 %4u KiB: %lu.%02lu MiB/s (%lld us)",
            pair->name, (unsigned)(bytes / KIB),
            (unsigned long)(mib_per_sec_100 / 100),
            (unsigned long)(mib_per_sec_100 % 100), (long long)best_us);
}

static bool allocate_pair(memory_pair_t *pair, size_t capacity,
        uint32_t capabilities)
{
    pair->source = heap_caps_aligned_alloc(64, capacity, capabilities);
    pair->target = heap_caps_aligned_alloc(64, capacity, capabilities);
    if (pair->source && pair->target)
    {
        pair->capacity = capacity;
        for (size_t byte = 0; byte < capacity; byte++)
            pair->source[byte] = (uint8_t)(byte * 131U + 17U);
        memset(pair->target, 0, capacity);
        return true;
    }
    heap_caps_free(pair->source);
    heap_caps_free(pair->target);
    pair->source = NULL;
    pair->target = NULL;
    return false;
}

static void run_pair(const memory_pair_t *pair)
{
    static const size_t sizes[] = {
        4U * KIB, 16U * KIB, 32U * KIB, 64U * KIB,
        80U * KIB, 96U * KIB, 112U * KIB, 128U * KIB,
        144U * KIB, 160U * KIB, 192U * KIB, 256U * KIB,
        512U * KIB, MIB,
    };
    for (unsigned index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++)
    {
        size_t bytes = sizes[index];
        if (bytes > pair->capacity)
            break;
        run_case(pair, bytes, READ);
        if (pair->evict_read)
            run_evicted_read(pair, bytes);
        run_case(pair, bytes, WRITE);
        run_case(pair, bytes, COPY);
    }
}

void memory_benchmark_run(void)
{
    if (xPortGetCoreID() != 0)
    {
        ESP_LOGE(TAG, "Benchmark requires core0");
        return;
    }
    ESP_LOGI(TAG, "CPU=%u MHz, L2 cache=%u KiB, PSRAM=%u MiB, best of %u",
            CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            CONFIG_CACHE_L2_CACHE_SIZE / KIB,
            (unsigned)(esp_psram_get_size() / MIB), SAMPLES);
    ESP_LOGI(TAG, "Warm scans transfer at least 4 MiB; cold32 follows a 1 MiB PSRAM scan");

    memory_pair_t sram = {.name = "SRAM"};
    memory_pair_t psram = {.name = "PSRAM", .evict_read = true};
    if (!allocate_pair(&sram, 128U * KIB,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) &&
            !allocate_pair(&sram, 64U * KIB,
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT))
        ESP_LOGE(TAG, "SRAM buffer allocation failed");

    if (esp_psram_get_size() &&
            !allocate_pair(&psram, MIB, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
        ESP_LOGE(TAG, "PSRAM buffer allocation failed");

    if (sram.capacity)
        run_pair(&sram);
    if (psram.capacity)
        run_pair(&psram);
    heap_caps_free(sram.source);
    heap_caps_free(sram.target);
    heap_caps_free(psram.source);
    heap_caps_free(psram.target);
}
