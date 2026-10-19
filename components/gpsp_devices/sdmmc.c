/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_sdmmc.h"
#include "gpsp_hardware.h"
#include "sdkconfig.h"
#if CONFIG_GPSP_SD_CARD
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "esp_timer.h"
#if CONFIG_GPSP_DCACHE_AUTOLOAD
#include "esp_cache.h"
#include "esp_memory_utils.h"
#include "soc/cache_reg.h"
#include "soc/soc.h"
#endif

static const char *TAG = "gpsp_sdmmc";
static const char *s_path;
static sdmmc_card_t *s_sd_card;
#if CONFIG_GPSP_SD_LDO_CHAN >= 0
static sd_pwr_ctrl_handle_t s_sd_power;
#endif

#if CONFIG_GPSP_DCACHE_AUTOLOAD
/* A fill started before DMA can finish after the driver's invalidation.
   Drain those fills before invalidating the receive buffer again. */
static esp_err_t gpsp_autoload_settle(void *buff, size_t size)
{
    uint32_t ctrl = REG_READ(CACHE_L1_DCACHE_AUTOLOAD_CTRL_REG);
    esp_err_t result = ESP_OK;

    if (!(ctrl & CACHE_L1_DCACHE_AUTOLOAD_ENA))
        return ESP_OK;
    REG_WRITE(CACHE_L1_DCACHE_AUTOLOAD_CTRL_REG,
            ctrl & ~(CACHE_L1_DCACHE_AUTOLOAD_ENA | CACHE_L1_DCACHE_AUTOLOAD_DONE));
    int64_t deadline = esp_timer_get_time() + 1000;
    while (!(REG_READ(CACHE_L1_DCACHE_AUTOLOAD_CTRL_REG) &
            CACHE_L1_DCACHE_AUTOLOAD_DONE))
    {
        if (esp_timer_get_time() >= deadline)
        {
            result = ESP_ERR_TIMEOUT;
            break;
        }
    }
    if (result == ESP_OK)
        result = esp_cache_msync(buff, size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    REG_WRITE(CACHE_L1_DCACHE_AUTOLOAD_CTRL_REG,
            ctrl & ~CACHE_L1_DCACHE_AUTOLOAD_DONE);
    return result;
}

/* The host hook also covers card initialization and filesystem mounting.
   cmd->data is the actual DMA buffer, including the driver's bounce buffer. */
static esp_err_t gpsp_sd_do_transaction(int slot, sdmmc_command_t *cmd)
{
    esp_err_t result = sdmmc_host_do_transaction(slot, cmd);
    size_t align;

    if (result != ESP_OK || cmd->error != ESP_OK ||
            !(cmd->flags & SCF_CMD_READ) || !cmd->data || !cmd->buflen ||
            !esp_ptr_external_ram(cmd->data))
        return result;

    align = esp_cache_get_line_size_by_addr(cmd->data);
    if (!align || (uintptr_t)cmd->data % align || cmd->buflen % align)
        return ESP_ERR_INVALID_ARG;
    result = gpsp_autoload_settle(cmd->data, cmd->buflen);
    if (result != ESP_OK)
        ESP_LOGE(TAG, "SD receive cache sync failed: %s",
                esp_err_to_name(result));
    return result;
}
#endif

/* Slot 0 has fixed pins on the P4. ESP-Hosted may already own the SDMMC
   controller for slot 1; the host init and slot deinit both allow that. */
bool gpsp_sdmmc_mount(const char *path)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    s_path = path;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_err_t result;

#if CONFIG_FATFS_VOLUME_COUNT < 3
    /* The card would take the volume that the save partition needs. */
    ESP_LOGW(TAG, "SD card skipped: needs CONFIG_FATFS_VOLUME_COUNT >= 3");
    return false;
#endif

#if CONFIG_GPSP_SD_LDO_CHAN >= 0
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = CONFIG_GPSP_SD_LDO_CHAN,
    };

    result = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_sd_power);
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "SD power setup failed: %s", esp_err_to_name(result));
        return false;
    }
    host.pwr_ctrl_handle = s_sd_power;
#endif

    host.slot = gpsp_hardware.sd.slot;
    /* 40 MHz high speed; card init falls back when a card lacks it. */
    host.max_freq_khz = gpsp_hardware.sd.frequency_khz;
    slot_config.width = gpsp_hardware.sd.width;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#if CONFIG_GPSP_DCACHE_AUTOLOAD
    host.do_transaction = gpsp_sd_do_transaction;
#endif

    result = esp_vfs_fat_sdmmc_mount(s_path, &host, &slot_config,
            &mount_config, &s_sd_card);
    if (result != ESP_OK)
    {
        ESP_LOGI(TAG, "No SD card: %s", esp_err_to_name(result));
        s_sd_card = NULL;
#if CONFIG_GPSP_SD_LDO_CHAN >= 0
        sd_pwr_ctrl_del_on_chip_ldo(s_sd_power);
        s_sd_power = NULL;
#endif
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted: %s", s_sd_card->cid.name);
    return true;
}

void gpsp_sdmmc_unmount(void)
{
    if (!s_sd_card)
        return;

    /* With ESP-Hosted on slot 1 the controller stays; IDF logs its refused
       delete as an error. */
    esp_log_level_t level = esp_log_level_get("SD_HOST");

    esp_log_level_set("SD_HOST", ESP_LOG_NONE);
    esp_vfs_fat_sdcard_unmount(s_path, s_sd_card);
    esp_log_level_set("SD_HOST", level);
    s_sd_card = NULL;
#if CONFIG_GPSP_SD_LDO_CHAN >= 0
    sd_pwr_ctrl_del_on_chip_ldo(s_sd_power);
    s_sd_power = NULL;
#endif
}

bool gpsp_sdmmc_mounted(void)
{
    return s_sd_card != NULL;
}

uint8_t gpsp_sdmmc_drive(void)
{
    return s_sd_card ? ff_diskio_get_pdrv_card(s_sd_card) : 0xff;
}
#endif
