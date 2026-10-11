/* Executable memory synchronization and common result reporting.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "probe.h"

#include "esp_cache.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "isa_probe";

bool probe_sync(void *memory, size_t size)
{
    esp_err_t err = esp_cache_msync(memory, size,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGE(TAG, "Data cache writeback failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_cache_msync(memory, size,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGE(TAG, "Instruction cache invalidation failed: %s", esp_err_to_name(err));
        return false;
    }

    __asm__ volatile("fence.i" ::: "memory");
    return true;
}

void probe_report(const char *group, probe_status_t status,
        unsigned passed, unsigned total, const char *detail)
{
    static const char *const names[] = {
        "PASS", "FAIL", "UNAVAILABLE", "SKIPPED"
    };
    const char *name = names[status];
    if (detail)
        ESP_LOGI(TAG, "%s PROBE RESULT: %s (%s)", group, name, detail);
    else if (status == PROBE_PASS || status == PROBE_FAIL)
        ESP_LOGI(TAG, "%s PROBE RESULT: %s (%u/%u cases)", group, name,
                passed, total);
    else
        ESP_LOGI(TAG, "%s PROBE RESULT: %s", group, name);
}
