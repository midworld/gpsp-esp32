/* ESP32 JIT memory probe.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_cache.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define PROBE_ALIGNMENT 256
#define PROBE_SIZE 256
#define MIB (1024u * 1024u)
#define KIB 1024u
#define SMALL_ROM_CACHE_SIZE (2 * MIB)
#define SMALL_RAM_CACHE_SIZE (384 * KIB)
#define FULL_ROM_CACHE_SIZE (10 * MIB)
#define FULL_RAM_CACHE_SIZE (512 * KIB)

static const char *TAG = "jit_probe";

#ifdef MALLOC_CAP_EXEC
static uint32_t addi_a0(unsigned amount)
{
    /* addi a0, a0, amount */
    return ((amount & 0xfff) << 20) | (10 << 15) | (10 << 7) | 0x13;
}

static bool synchronize_code(void *code, bool require_cache_sync)
{
    esp_err_t err = esp_cache_msync(code, PROBE_SIZE,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != ESP_OK && (require_cache_sync || err != ESP_ERR_NOT_SUPPORTED))
    {
        ESP_LOGE(TAG, "Data cache writeback failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_cache_msync(code, PROBE_SIZE,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    if (err != ESP_OK && (require_cache_sync || err != ESP_ERR_NOT_SUPPORTED))
    {
        ESP_LOGE(TAG, "Instruction cache invalidation failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Execute on the same core that wrote the instructions. */
    __asm__ volatile("fence.i" ::: "memory");
    return true;
}

static bool test_code_at(uint8_t *code, bool require_cache_sync)
{
    uint32_t *instructions = (uint32_t *)code;
    instructions[0] = addi_a0(1);
    instructions[1] = 0x00008067; /* ret = jalr x0, 0(ra) */

    if (!synchronize_code(code, require_cache_sync))
        return false;

    int (*generated)(int) = (int (*)(int))code;
    int first = generated(41);
    instructions[0] = addi_a0(2);

    if (!synchronize_code(code, require_cache_sync))
        return false;

    int patched = generated(41);
    return first == 42 && patched == 43;
}

static bool test_cache(const char *name, uint8_t *cache, size_t size,
        bool require_cache_sync)
{
    size_t offsets[3] = {
        0,
        (size / 2) & ~(size_t)(PROBE_ALIGNMENT - 1),
        (size - PROBE_SIZE) & ~(size_t)(PROBE_ALIGNMENT - 1),
    };

    for (unsigned i = 0; i < 3; i++)
    {
        if (i > 0 && offsets[i] == offsets[i - 1])
            continue;

        ESP_LOGI(TAG, "%s offset=%u code=%p: running", name,
                (unsigned)offsets[i], (void *)(cache + offsets[i]));
        bool passed = test_code_at(cache + offsets[i], require_cache_sync);
        ESP_LOGI(TAG, "%s offset=%u code=%p: %s", name, (unsigned)offsets[i],
                (void *)(cache + offsets[i]), passed ? "PASS" : "FAIL");
        if (!passed)
            return false;
    }
    return true;
}

static bool test_internal(void)
{
    const uint32_t caps = MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL;
    ESP_LOGI(TAG, "internal EXEC free=%u largest=%u bytes",
            (unsigned)heap_caps_get_free_size(caps),
            (unsigned)heap_caps_get_largest_free_block(caps));

    uint8_t *code = heap_caps_aligned_alloc(PROBE_ALIGNMENT, PROBE_SIZE, caps);
    if (!code)
    {
        ESP_LOGE(TAG, "internal EXEC allocation failed");
        return false;
    }

    bool passed = test_cache("internal", code, PROBE_SIZE, false);
    heap_caps_free(code);
    return passed;
}

static bool test_psram_pair(const char *name, size_t rom_size, size_t ram_size)
{
    ESP_LOGI(TAG, "%s: PSRAM largest=%u ROM=%u RAM=%u bytes", name,
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
            (unsigned)rom_size, (unsigned)ram_size);

    uint8_t *rom = heap_caps_aligned_alloc(PROBE_ALIGNMENT, rom_size,
            MALLOC_CAP_SPIRAM);
    uint8_t *ram = heap_caps_aligned_alloc(PROBE_ALIGNMENT, ram_size,
            MALLOC_CAP_SPIRAM);
    if (!rom || !ram)
    {
        ESP_LOGE(TAG, "%s: simultaneous cache allocation failed (ROM=%p RAM=%p)",
                name, (void *)rom, (void *)ram);
        heap_caps_free(ram);
        heap_caps_free(rom);
        return false;
    }

    ESP_LOGI(TAG, "%s: allocated ROM=%p RAM=%p, line=%u bytes", name,
            (void *)rom, (void *)ram,
            (unsigned)esp_cache_get_line_size_by_addr(rom));

    bool rom_ok = test_cache("ROM", rom, rom_size, true);
    bool ram_ok = test_cache("RAM", ram, ram_size, true);
    heap_caps_free(ram);
    heap_caps_free(rom);

    ESP_LOGI(TAG, "%s cache pair: %s", name, rom_ok && ram_ok ? "PASS" : "FAIL");
    return rom_ok && ram_ok;
}

static bool test_contiguous_cache(size_t rom_size, size_t ram_size)
{
    size_t total_size = rom_size + ram_size;
    ESP_LOGI(TAG, "contiguous: PSRAM largest=%u total=%u bytes",
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
            (unsigned)total_size);

    uint8_t *rom = heap_caps_aligned_alloc(PROBE_ALIGNMENT, total_size,
            MALLOC_CAP_SPIRAM);
    if (!rom)
    {
        ESP_LOGE(TAG, "contiguous: cache allocation failed");
        return false;
    }

    uint8_t *ram = rom + rom_size;
    ESP_LOGI(TAG, "contiguous: ROM=%p RAM=%p, line=%u bytes",
            (void *)rom, (void *)ram,
            (unsigned)esp_cache_get_line_size_by_addr(rom));

    bool rom_ok = test_cache("contiguous ROM", rom, rom_size, true);
    bool ram_ok = test_cache("contiguous RAM", ram, ram_size, true);
    heap_caps_free(rom);

    ESP_LOGI(TAG, "contiguous cache: %s", rom_ok && ram_ok ? "PASS" : "FAIL");
    return rom_ok && ram_ok;
}
#endif

static bool jit_probe_run(void)
{
    ESP_LOGI(TAG, "PSRAM free=%u largest=%u bytes",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

#ifdef MALLOC_CAP_EXEC
    ESP_LOGI(TAG, "EXEC|SPIRAM largest=%u bytes",
            (unsigned)heap_caps_get_largest_free_block(
                    MALLOC_CAP_EXEC | MALLOC_CAP_SPIRAM));
    bool internal_ok = test_internal();
    bool small_ok = test_psram_pair("small",
            SMALL_ROM_CACHE_SIZE, SMALL_RAM_CACHE_SIZE);
    bool full_ok = test_psram_pair("full",
            FULL_ROM_CACHE_SIZE, FULL_RAM_CACHE_SIZE);
    bool contiguous_ok = test_contiguous_cache(
            FULL_ROM_CACHE_SIZE, FULL_RAM_CACHE_SIZE);

    bool passed = internal_ok && small_ok && full_ok && contiguous_ok;
    ESP_LOGI(TAG, "JIT PROBE RESULT: %s (internal=%s small=%s full=%s contiguous=%s)",
            passed ? "PASS" : "FAIL", internal_ok ? "PASS" : "FAIL",
            small_ok ? "PASS" : "FAIL", full_ok ? "PASS" : "FAIL",
            contiguous_ok ? "PASS" : "FAIL");
    ESP_LOGI(TAG, "JIT PROBE COMPLETE");
    return passed;
#else
    ESP_LOGW(TAG, "MALLOC_CAP_EXEC unavailable with memory protection enabled");
    ESP_LOGI(TAG, "JIT PROBE RESULT: UNAVAILABLE (memory protection enabled)");
    ESP_LOGI(TAG, "JIT PROBE COMPLETE");
    return false;
#endif
}

void app_main(void)
{
    jit_probe_run();
}
