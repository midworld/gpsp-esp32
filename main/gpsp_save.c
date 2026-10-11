/* Cartridge save persistence: two checked slots on the flash save partition,
 * or a raw .sav file on the SD card for a card ROM.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_save.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "gpsp_backup.h"
#include "gpsp_save_file.h"
#include "gpsp_storage.h"
#include "libretro.h"

#define GPSP_SAVE_MAGIC 0x47505350u
#define GPSP_SAVE_INTERVAL_US 5000000

struct gpsp_save_header
{
    uint32_t magic;
    uint32_t rom_hash;
    uint32_t generation;
    uint32_t size;
    uint32_t data_hash;
};

static const char *TAG = "gpsp_save";
static char s_paths[2][40];
static const char *s_raw_path;
static uint8_t *s_memory;
static uint8_t *s_snapshot;
static size_t s_size;
static size_t s_snapshot_size;
static size_t s_expected_size;
static uint32_t s_rom_hash;
static uint32_t s_generation;
static int s_active_slot;
static int64_t s_last_poll_us;
static TaskHandle_t s_worker;
static SemaphoreHandle_t s_worker_done;
static atomic_bool s_busy;
static atomic_bool s_stop;
static bool s_snapshot_saved;
/* Files on the card replaced the cartridge under the game. */
static bool s_disabled;

static uint32_t gpsp_save_hash(const uint8_t *data, size_t size,
        uint32_t hash)
{
    for (size_t i = 0; i < size; i++)
        hash = (hash ^ data[i]) * 16777619u;
    return hash;
}

static bool gpsp_save_rom_hash(const char *path, uint32_t *hash)
{
    uint8_t buffer[4096];
    FILE *file = fopen(path, "rb");
    size_t count;

    if (!file)
        return false;

    *hash = 2166136261u;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
        *hash = gpsp_save_hash(buffer, count, *hash);

    bool valid = !ferror(file);
    if (fclose(file) != 0)
        valid = false;
    return valid;
}

static void gpsp_save_load_from(char (*paths)[40])
{
    for (int slot = 0; slot < 2; slot++)
    {
        struct gpsp_save_header header;
        FILE *file = fopen(paths[slot], "rb");
        bool valid;

        if (!file)
            continue;

        valid = fread(&header, sizeof(header), 1, file) == 1 &&
                header.magic == GPSP_SAVE_MAGIC &&
                header.rom_hash == s_rom_hash &&
                header.size == s_size &&
                fread(s_snapshot, s_size, 1, file) == 1 &&
                gpsp_save_hash(s_snapshot, s_size, 2166136261u) ==
                        header.data_hash;
        fclose(file);

        if (!valid || (s_active_slot >= 0 &&
                (int32_t)(header.generation - s_generation) <= 0))
            continue;

        memcpy(s_memory, s_snapshot, s_size);
        s_generation = header.generation;
        s_active_slot = slot;
    }

    if (s_active_slot >= 0)
        ESP_LOGI(TAG, "Cartridge save loaded: %s", paths[s_active_slot]);
}

/* A raw .sav may be shorter than the save memory, as for EEPROM. */
static bool gpsp_save_read_raw(const char *path)
{
    size_t size = gpsp_save_file_read(path, s_snapshot, s_size,
            s_expected_size);

    if (!size)
        return false;

    memcpy(s_memory, s_snapshot, size);
    ESP_LOGI(TAG, "Cartridge save loaded: %s (%u bytes)", path,
            (unsigned)size);
    return true;
}

static void gpsp_save_load(void)
{
    if (s_raw_path)
    {
        char backup[300];

        snprintf(backup, sizeof(backup), "%s.bak", s_raw_path);
        /* Without a card save, the flash save of this ROM is used once. */
        if (!gpsp_save_read_raw(s_raw_path) && !gpsp_save_read_raw(backup))
        {
            gpsp_save_load_from(s_paths);
            s_active_slot = -1;
        }
    }
    else
        gpsp_save_load_from(s_paths);

    memcpy(s_snapshot, s_memory, s_size);
}

static bool gpsp_save_write_raw(void)
{
    if (!gpsp_save_file_write(s_raw_path, s_snapshot, s_snapshot_size))
        return false;

    ESP_LOGI(TAG, "Cartridge save written: %s (%u bytes)", s_raw_path,
            (unsigned)s_snapshot_size);
    return true;
}

static bool gpsp_save_write(void)
{
    if (s_raw_path)
        return gpsp_save_write_raw();

    int slot = s_active_slot == 0 ? 1 : 0;
    struct gpsp_save_header header = {
        .magic = GPSP_SAVE_MAGIC,
        .rom_hash = s_rom_hash,
        .generation = s_generation + 1,
        .size = (uint32_t)s_size,
        .data_hash = gpsp_save_hash(s_snapshot, s_size, 2166136261u),
    };
    FILE *file = fopen(s_paths[slot], "wb");
    bool valid;

    if (!file)
        return false;

    valid = fwrite(&header, sizeof(header), 1, file) == 1 &&
            fwrite(s_snapshot, s_size, 1, file) == 1;
    if (fclose(file) != 0)
        valid = false;
    if (!valid)
    {
        remove(s_paths[slot]);
        return false;
    }

    s_active_slot = slot;
    s_generation = header.generation;
    ESP_LOGI(TAG, "Cartridge save written: %s", s_paths[slot]);
    return true;
}

static void gpsp_save_worker(void *arg)
{
    (void)arg;

    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (atomic_load_explicit(&s_stop, memory_order_acquire))
            break;

        s_snapshot_saved = gpsp_save_write();
        if (!s_snapshot_saved)
            ESP_LOGE(TAG, "Failed to write cartridge save");
        atomic_store_explicit(&s_busy, false, memory_order_release);
    }

    xSemaphoreGive(s_worker_done);
    vTaskDelete(NULL);
}

esp_err_t gpsp_save_start(const char *rom_path)
{
    s_disabled = false;
    s_memory = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    s_size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!s_memory || !s_size || !gpsp_save_rom_hash(rom_path, &s_rom_hash))
        return ESP_FAIL;

    s_snapshot = heap_caps_malloc(s_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_snapshot)
        return ESP_ERR_NO_MEM;

    s_raw_path = gpsp_storage_save_path();
    /* The ROM's detected save type; zero accepts any standard size. */
    s_expected_size = gpsp_backup_size();
    snprintf(s_paths[0], sizeof(s_paths[0]), "/saves/%08lx.s0",
            (unsigned long)s_rom_hash);
    snprintf(s_paths[1], sizeof(s_paths[1]), "/saves/%08lx.s1",
            (unsigned long)s_rom_hash);
    s_active_slot = -1;
    s_generation = 0;
    s_snapshot_saved = true;
    atomic_store(&s_busy, false);
    atomic_store(&s_stop, false);
    gpsp_save_load();
    s_last_poll_us = esp_timer_get_time();

    s_worker_done = xSemaphoreCreateBinary();
    if (!s_worker_done || xTaskCreate(gpsp_save_worker, "gpsp_save", 4096,
            NULL, tskIDLE_PRIORITY + 1, &s_worker) != pdPASS)
    {
        if (s_worker_done)
            vSemaphoreDelete(s_worker_done);
        s_worker_done = NULL;
        heap_caps_free(s_snapshot);
        s_snapshot = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* Queues a write when save RAM changed; force skips the interval. */
static void gpsp_save_check(bool force)
{
    int64_t now_us = esp_timer_get_time();

    if (!s_worker || s_disabled ||
            (!force && now_us - s_last_poll_us < GPSP_SAVE_INTERVAL_US))
        return;
    s_last_poll_us = now_us;

    if (atomic_load_explicit(&s_busy, memory_order_acquire) ||
            (s_snapshot_saved && memcmp(s_memory, s_snapshot, s_size) == 0))
        return;

    memcpy(s_snapshot, s_memory, s_size);
    /* Fix the size with the snapshot; the worker must not read the core. */
    s_snapshot_size = gpsp_backup_size();
    if (!s_snapshot_size || s_snapshot_size > s_size)
        s_snapshot_size = s_size;
    atomic_store_explicit(&s_busy, true, memory_order_release);
    xTaskNotifyGive(s_worker);
}

void gpsp_save_poll(void)
{
    gpsp_save_check(false);
}

void gpsp_save_disable(void)
{
    s_disabled = true;
}

void gpsp_save_flush(void)
{
    gpsp_save_check(true);
}

void gpsp_save_wait(void)
{
    while (atomic_load_explicit(&s_busy, memory_order_acquire))
        vTaskDelay(1);
}

void gpsp_save_stop(void)
{
    if (!s_worker)
        return;

    gpsp_save_wait();
    /* The last change is written even within the poll interval. */
    gpsp_save_check(true);
    gpsp_save_wait();

    atomic_store_explicit(&s_stop, true, memory_order_release);
    xTaskNotifyGive(s_worker);
    xSemaphoreTake(s_worker_done, portMAX_DELAY);
    vSemaphoreDelete(s_worker_done);
    s_worker_done = NULL;
    s_worker = NULL;
    heap_caps_free(s_snapshot);
    s_snapshot = NULL;
}
