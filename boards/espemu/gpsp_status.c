/* CPU use, game speed, memory and temperature reports.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_status.h"

#include "sdkconfig.h"

#if CONFIG_GPSP_STATUS

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_private/freertos_idf_additions_priv.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gpsp_frame_stats.h"
#include "gpsp_profile.h"

#if CONFIG_GPSP_LCD
#include "p4_nano_lcd.h"
#endif
#if CONFIG_GPSP_AUDIO
#include "p4_nano_audio.h"
#endif

#define GPSP_STATUS_PERIOD_MS 2000
/* GBA frames per second, in millionths of a frame. */
#define GPSP_STATUS_GBA_FPS_MICRO 59727500ull

#if CONFIG_FREERTOS_UNICORE
#define GPSP_STATUS_CORES 1
#define GPSP_STATUS_TASK_CORE tskNO_AFFINITY
#else
#define GPSP_STATUS_CORES 2
#define GPSP_STATUS_TASK_CORE 1
#endif

static const char *TAG = "gpsp_status";
static temperature_sensor_handle_t s_sensor;
static TaskHandle_t s_task;
static atomic_bool s_running;
static atomic_bool s_tasks_requested;

/* One task as copied out of a snapshot. */
struct gpsp_status_task_sample
{
    TaskHandle_t handle;
    UBaseType_t number;
    BaseType_t core;
    uint32_t run;
    uint32_t stack_free;
    char name[configMAX_TASK_NAME_LEN];
};

/* pcTaskName points into the task, which is freed once deleted. The
   kernel lock keeps every listed task alive until the names are copied:
   vTaskDelete() unlists a task under the same lock before freeing it. */
static void gpsp_status_lock(void)
{
#if !CONFIG_FREERTOS_SMP && !CONFIG_FREERTOS_UNICORE
    prvTakeKernelLock();
#else
    vTaskSuspendAll();
#endif
}

static void gpsp_status_unlock(void)
{
#if !CONFIG_FREERTOS_SMP && !CONFIG_FREERTOS_UNICORE
    prvReleaseKernelLock();
#else
    xTaskResumeAll();
#endif
}

/* Tasks waiting for deletion are skipped without reading their names. */
static UBaseType_t gpsp_status_sample(TaskStatus_t *raw, UBaseType_t capacity,
        struct gpsp_status_task_sample *out, uint32_t *total)
{
    configRUN_TIME_COUNTER_TYPE total_run = 0;
    UBaseType_t count;
    UBaseType_t used = 0;

    gpsp_status_lock();
    count = uxTaskGetSystemState(raw, capacity, &total_run);

    for (UBaseType_t i = 0; i < count; i++)
    {
        if (raw[i].eCurrentState == eDeleted)
            continue;
        out[used].handle = raw[i].xHandle;
        out[used].number = raw[i].xTaskNumber;
        out[used].core = raw[i].xCoreID;
        out[used].run = (uint32_t)raw[i].ulRunTimeCounter;
        out[used].stack_free = (uint32_t)raw[i].usStackHighWaterMark;
        strlcpy(out[used].name, raw[i].pcTaskName, sizeof(out[used].name));
        used++;
    }
    gpsp_status_unlock();
    *total = (uint32_t)total_run;
    return used;
}

/* CPU share of each task over one second. Deltas of the 32-bit counters
   stay correct when they wrap (about every 71 minutes). */
static void gpsp_status_log_tasks(void)
{
    UBaseType_t capacity = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *raw = malloc(capacity * sizeof(*raw));
    struct gpsp_status_task_sample *before = malloc(capacity * sizeof(*before));
    struct gpsp_status_task_sample *after = malloc(capacity * sizeof(*after));
    uint32_t total_before;
    uint32_t total_after;
    UBaseType_t count_before;
    UBaseType_t count_after;

    if (!raw || !before || !after)
        goto done;

    count_before = gpsp_status_sample(raw, capacity, before, &total_before);
    vTaskDelay(pdMS_TO_TICKS(1000));
    count_after = gpsp_status_sample(raw, capacity, after, &total_after);
    uint32_t total = total_after - total_before;

    ESP_LOGI(TAG, "TASK name             core  cpu%%  stack_free");
    for (UBaseType_t i = 0; i < count_after; i++)
    {
        char share[8] = "-";

        /* A handle can be reused by a new task; the number tells them apart.
           New tasks have no first sample. */
        for (UBaseType_t j = 0; j < count_before; j++)
            if (before[j].handle == after[i].handle &&
                    before[j].number == after[i].number && total)
            {
                snprintf(share, sizeof(share), "%.1f",
                        (after[i].run - before[j].run) * 100.0 / total);
                break;
            }
        ESP_LOGI(TAG, "TASK %-16s %4s %5s %8u", after[i].name,
                after[i].core == tskNO_AFFINITY ? "-" :
                after[i].core == 0 ? "0" : "1", share,
                (unsigned)after[i].stack_free);
    }

done:
    free(raw);
    free(before);
    free(after);
}

/* Busy share of the period: the rest is the core's idle task. */
static unsigned gpsp_status_cpu(int core, configRUN_TIME_COUNTER_TYPE *idle,
        uint32_t elapsed_us)
{
    configRUN_TIME_COUNTER_TYPE now = ulTaskGetIdleRunTimeCounterForCore(core);
    uint32_t idle_us = (uint32_t)(now - *idle);

    *idle = now;
    if (!elapsed_us || idle_us >= elapsed_us)
        return 0;
    return 100u - (unsigned)((uint64_t)idle_us * 100u / elapsed_us);
}

static void gpsp_status_task(void *arg)
{
    configRUN_TIME_COUNTER_TYPE idle[GPSP_STATUS_CORES];
    int64_t start_us = esp_timer_get_time();
    uint32_t start_frames = gpsp_profile_frames();
    uint32_t start_rom[4];

    (void)arg;
    gpsp_rom_page_stats(&start_rom[0], &start_rom[1], &start_rom[2],
            &start_rom[3]);
    for (int core = 0; core < GPSP_STATUS_CORES; core++)
        idle[core] = ulTaskGetIdleRunTimeCounterForCore(core);

    while (atomic_load(&s_running))
    {
        unsigned cpu[2] = {0, 0};
        float celsius = 0.0f;
        bool have_celsius;
        char temp_text[12] = "n/a";
        int64_t now_us;
        uint32_t elapsed_us;
        uint32_t frames;
        unsigned speed;

        /* F2 requests an early sample and a task dump. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(GPSP_STATUS_PERIOD_MS));
        if (atomic_exchange(&s_tasks_requested, false))
            gpsp_status_log_tasks();
        now_us = esp_timer_get_time();
        elapsed_us = (uint32_t)(now_us - start_us);
        start_us = now_us;
        /* Emulated frames against the GBA rate; skipped frames count. */
        frames = gpsp_profile_frames();
        speed = elapsed_us ? (unsigned)(((uint64_t)(frames - start_frames) *
                100000000000000ull / elapsed_us + GPSP_STATUS_GBA_FPS_MICRO / 2) /
                GPSP_STATUS_GBA_FPS_MICRO) : 0;
        start_frames = frames;
        for (int core = 0; core < GPSP_STATUS_CORES; core++)
            cpu[core] = gpsp_status_cpu(core, &idle[core], elapsed_us);

        have_celsius = s_sensor &&
                temperature_sensor_get_celsius(s_sensor, &celsius) == ESP_OK;
        if (have_celsius)
            snprintf(temp_text, sizeof(temp_text), "%.1fC", celsius);

        size_t sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t sram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t sram_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

        uint32_t rom[4];

        gpsp_rom_page_stats(&rom[0], &rom[1], &rom[2], &rom[3]);
        char rom_text[80];
        snprintf(rom_text, sizeof(rom_text),
                " rom_pages=%u rom_reloads=%u rom_remaps=%u rom_load=%ums",
                (unsigned)(rom[0] - start_rom[0]),
                (unsigned)(rom[1] - start_rom[1]),
                (unsigned)(rom[2] - start_rom[2]),
                (unsigned)((rom[3] - start_rom[3]) /
                        (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000)));
        memcpy(start_rom, rom, sizeof(start_rom));

        char audio_text[56] = "";
#if CONFIG_GPSP_AUDIO
        unsigned audio_ms;
        unsigned underruns;
        unsigned i2s_overflows;

        gpsp_p4_nano_audio_stats(&audio_ms, &underruns, &i2s_overflows);
        snprintf(audio_text, sizeof(audio_text),
                " audio=%ums underrun=%u i2s_ovf=%u", audio_ms, underruns,
                i2s_overflows);
#endif

        /* All memory values are free bytes; *_min is the lowest since boot. */
        ESP_LOGI(TAG, "STAT cpu0=%u%% cpu1=%u%% speed=%u%% sram_free=%uK "
                "sram_min=%uK sram_blk=%uK psram_free=%.1fM psram_min=%.1fM "
                "temp=%s%s%s", cpu[0], cpu[1], speed, (unsigned)(sram / 1024),
                (unsigned)(sram_min / 1024), (unsigned)(sram_block / 1024),
                psram / 1048576.0, psram_min / 1048576.0, temp_text, rom_text,
                audio_text);
#if CONFIG_GPSP_LCD
        /* Used PSRAM includes reserved memory. */
        size_t psram_size = esp_psram_get_size();
        size_t psram_used = psram_size > psram ? psram_size - psram : 0;

        gpsp_p4_nano_lcd_status(cpu[0], cpu[1],
                have_celsius ? (int)(celsius * 10.0f) : INT_MIN, speed,
                (unsigned)(sram / 1024), (unsigned)(sram_min / 1024),
                (unsigned)((psram_used * 10 + 524288) / 1048576),
                (unsigned)(psram_size / 1048576));
#endif
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

void gpsp_status_start(void)
{
    const temperature_sensor_config_t sensor_config =
            TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);

    if (s_task)
        return;
    if (temperature_sensor_install(&sensor_config, &s_sensor) != ESP_OK ||
            temperature_sensor_enable(s_sensor) != ESP_OK)
    {
        ESP_LOGW(TAG, "Temperature sensor unavailable");
        if (s_sensor)
            temperature_sensor_uninstall(s_sensor);
        s_sensor = NULL;
    }

    atomic_store(&s_running, true);
    if (xTaskCreatePinnedToCore(gpsp_status_task, "gpsp_status", 3072, NULL,
            tskIDLE_PRIORITY + 2, &s_task, GPSP_STATUS_TASK_CORE) != pdPASS)
    {
        ESP_LOGW(TAG, "Status task failed");
        atomic_store(&s_running, false);
        s_task = NULL;
        if (s_sensor)
        {
            temperature_sensor_disable(s_sensor);
            temperature_sensor_uninstall(s_sensor);
            s_sensor = NULL;
        }
    }
}

/* Wake the status task to print the task dump. */
void gpsp_status_print_tasks(void)
{
    if (!s_task)
        return;
    atomic_store(&s_tasks_requested, true);
    xTaskNotifyGive(s_task);
}

void gpsp_status_stop(void)
{
    if (!s_task)
        return;
    atomic_store(&s_running, false);
    while (s_task)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (s_sensor)
    {
        temperature_sensor_disable(s_sensor);
        temperature_sensor_uninstall(s_sensor);
        s_sensor = NULL;
    }
}

#endif
