/* Standalone ISA probe entry point.
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "probe.h"

#include "sdkconfig.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "isa_probe";

#if CONFIG_IDF_TARGET_ESP32S31
typedef struct {
    uint8_t *code;
    TaskHandle_t caller;
} pie_task_args_t;

static void pie_task(void *arg)
{
    pie_task_args_t *args = arg;
    probe_xespv_run(args->code);
    xTaskNotifyGive(args->caller);
    vTaskDelete(NULL);
}

static void run_pie_on_cpu1(uint8_t *code)
{
    pie_task_args_t args = {code, xTaskGetCurrentTaskHandle()};
    BaseType_t created = xTaskCreatePinnedToCore(pie_task, "isa_pie", 4096,
            &args, 5, NULL, 1);
    if (created != pdPASS)
    {
        probe_report("XESPV", PROBE_UNAVAILABLE, 0, 0, "CPU1 task creation");
        return;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
#endif

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "ISA PROBE START target=%s rev=%u.%u cores=%u core=%ld IDF=%s PIE mode=%d",
            CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100,
            chip.cores, (long)xPortGetCoreID(), esp_get_idf_version(),
            ISA_PROBE_PIE_CASE);

#ifdef MALLOC_CAP_EXEC
    uint8_t *code = heap_caps_aligned_alloc(PROBE_CODE_ALIGNMENT,
            PROBE_CODE_SIZE, MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    if (!code)
    {
        ESP_LOGE(TAG, "Executable internal RAM allocation failed");
        probe_report("ZCB", PROBE_UNAVAILABLE, 0, 0, NULL);
        probe_report("ZCMP", PROBE_UNAVAILABLE, 0, 0, NULL);
        probe_report("ZCMT", PROBE_UNAVAILABLE, 0, 0, NULL);
        probe_report("ZBA", PROBE_UNAVAILABLE, 0, 0, NULL);
        probe_report("ZBB", PROBE_UNAVAILABLE, 0, 0, NULL);
        probe_report("ZBS", PROBE_UNAVAILABLE, 0, 0, NULL);
        probe_report("RV32I", PROBE_UNAVAILABLE, 0, 0, NULL);
        probe_report("RV32M", PROBE_UNAVAILABLE, 0, 0, NULL);
        probe_report("RV32C", PROBE_UNAVAILABLE, 0, 0, NULL);
        probe_report("RV32A", PROBE_UNAVAILABLE, 0, 0, NULL);
        probe_report("RV32F", PROBE_UNAVAILABLE, 0, 0, NULL);
        probe_report("XESPLOOP", PROBE_UNAVAILABLE, 0, 0, NULL);
        probe_report("XESPV", PROBE_UNAVAILABLE, 0, 0, NULL);
    }
    else
    {
        probe_status_t ram_baseline = probe_core_baseline(code);
        if (ram_baseline != PROBE_PASS)
        {
            probe_report("RAM", ram_baseline, 0, 1, "RV32I addi baseline");
            probe_report("ZCB", PROBE_SKIPPED, 0, 0, "RAM baseline");
            probe_report("ZCMP", PROBE_SKIPPED, 0, 0, "RAM baseline");
            probe_report("ZCMT", PROBE_SKIPPED, 0, 0, "RAM baseline");
            probe_report("ZBA", PROBE_SKIPPED, 0, 0, "RAM baseline");
            probe_report("ZBB", PROBE_SKIPPED, 0, 0, "RAM baseline");
            probe_report("ZBS", PROBE_SKIPPED, 0, 0, "RAM baseline");
            probe_report("RV32I", PROBE_SKIPPED, 0, 0, "RAM baseline");
            probe_report("RV32M", PROBE_SKIPPED, 0, 0, "RAM baseline");
            probe_report("RV32C", PROBE_SKIPPED, 0, 0, "RAM baseline");
            probe_report("RV32A", PROBE_SKIPPED, 0, 0, "RAM baseline");
            probe_report("RV32F", PROBE_SKIPPED, 0, 0, "RAM baseline");
            probe_report("XESPLOOP", PROBE_SKIPPED, 0, 0, "RAM baseline");
            probe_report("XESPV", PROBE_SKIPPED, 0, 0, "RAM baseline");
        }
        else
        {
            probe_core_run(code);
            probe_af_run(code);
            probe_status_t c_baseline = probe_zcb_baseline(code);
            if (c_baseline == PROBE_PASS)
                probe_zcb_run(code);
            else
                probe_report("ZCB", c_baseline, 0, 0, "C baseline");
            probe_zcmp_run(code);
            probe_zcmt_run(code);
            probe_zb_run(code);
            probe_xesploop_run(code);
            ESP_LOGI(TAG, "ISA PROBE PRE-PIE COMPLETE");
#if CONFIG_IDF_TARGET_ESP32S31
            run_pie_on_cpu1(code);
#else
            probe_xespv_run(code);
#endif
        }
        heap_caps_free(code);
    }
#else
    ESP_LOGW(TAG, "MALLOC_CAP_EXEC is not available");
    probe_report("ZCB", PROBE_UNAVAILABLE, 0, 0, NULL);
    probe_report("ZCMP", PROBE_UNAVAILABLE, 0, 0, NULL);
    probe_report("ZCMT", PROBE_UNAVAILABLE, 0, 0, NULL);
    probe_report("ZBA", PROBE_UNAVAILABLE, 0, 0, NULL);
    probe_report("ZBB", PROBE_UNAVAILABLE, 0, 0, NULL);
    probe_report("ZBS", PROBE_UNAVAILABLE, 0, 0, NULL);
    probe_report("RV32I", PROBE_UNAVAILABLE, 0, 0, NULL);
    probe_report("RV32M", PROBE_UNAVAILABLE, 0, 0, NULL);
    probe_report("RV32C", PROBE_UNAVAILABLE, 0, 0, NULL);
    probe_report("RV32A", PROBE_UNAVAILABLE, 0, 0, NULL);
    probe_report("RV32F", PROBE_UNAVAILABLE, 0, 0, NULL);
    probe_report("XESPLOOP", PROBE_UNAVAILABLE, 0, 0, NULL);
    probe_report("XESPV", PROBE_UNAVAILABLE, 0, 0, NULL);
#endif
    ESP_LOGI(TAG, "ISA PROBE COMPLETE");
}
