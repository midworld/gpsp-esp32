/* ESP-IDF services for the gpSP RV32 JIT.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_rv32_esp_platform.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_private/esp_cache_private.h"
#include "hal/cache_ll.h"
#endif

static void *exec_alloc(size_t alignment, size_t size)
{
    if (!alignment || (alignment & (alignment - 1)) || !size)
        return NULL;

    void *code = NULL;
    /* PSRAM code publication needs cache-line alignment. */
    if (alignment < 256)
        alignment = 256;
    code = heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_SPIRAM);
    if (!code)
        return NULL;

    size_t line = esp_cache_get_line_size_by_addr(code);
    if (line > alignment || (line && alignment % line))
    {
        heap_caps_free(code);
        return NULL;
    }
    return code;
}

static void exec_free(void *code)
{
    heap_caps_free(code);
}

static bool exec_publish(void *code, size_t size)
{
    if (!code || size > UINTPTR_MAX - (uintptr_t)code)
        return false;
    if (!size)
        return true;

    uintptr_t first = (uintptr_t)code;
    uintptr_t limit = first + size;
    size_t line = esp_cache_get_line_size_by_addr(code);
    if (line)
    {
        if ((line & (line - 1)) || limit > UINTPTR_MAX - (line - 1))
            return false;
        first &= ~(uintptr_t)(line - 1);
        limit = (limit + line - 1) & ~(uintptr_t)(line - 1);
    }

    size = limit - first;
#if CONFIG_IDF_TARGET_ESP32P4
    /* PSRAM code: L2 serves both L1 caches, so writing back L1D and
       invalidating L1I is enough. One lock replaces two msync calls. */
    esp_cache_sync_ops_enter_critical_section();
    cache_ll_l1_writeback_dcache_addr(CACHE_LL_ID_ALL, first, size);
    cache_ll_l1_invalidate_icache_addr(CACHE_LL_ID_ALL, first, size);
    esp_cache_sync_ops_exit_critical_section();
#else
    esp_err_t err = esp_cache_msync((void *)first, size,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != ESP_OK && (err != ESP_ERR_NOT_SUPPORTED || line))
        return false;
    err = esp_cache_msync((void *)first, size,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    if (err != ESP_OK && (err != ESP_ERR_NOT_SUPPORTED || line))
        return false;
#endif

    /* Generated code is published and run on the same core. */
#if defined(__riscv)
    __asm__ volatile("fence.i" ::: "memory");
#else
    __sync_synchronize();
#endif
    return true;
}

static void vlog(gpsp_rv32_log_level level, const char *tag,
        const char *format, va_list args)
{
    char message[256];
    vsnprintf(message, sizeof(message), format, args);

    if (level == GPSP_RV32_LOG_WARN)
        ESP_LOGW(tag, "%s", message);
    else
        ESP_LOGI(tag, "%s", message);
}

const struct gpsp_rv32_platform_ops gpsp_esp_rv32_platform = {
    .exec_alloc = exec_alloc,
    .exec_free = exec_free,
    .exec_publish = exec_publish,
    .vlog = vlog,
};
