/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_sdmmc.h"
#include "gpsp_hardware.h"
#include "sdkconfig.h"
#if CONFIG_GPSP_SD_CARD
#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "esp_private/sd_host_private.h"
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
/* Under the FatFs volume lock: set by a failed recovery */
static bool s_card_failed;
/* Under the FatFs volume lock: multi-block writes are not used */
#if CONFIG_GPSP_SD_SINGLE_BLOCK_WRITES
static bool s_single_writes = true;
#else
static bool s_single_writes;
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

/* IDF v6.1 legacy host (legacy/src/sdmmc_internal.h) */
sd_host_slot_handle_t sdmmc_get_slot_handle(int slot_id);

/* After a timed-out data transfer IDF only stops the DMA: the controller
   keeps its data state and every later command times out. Resets the
   controller under the host's lock (the ESP-Hosted slot waits and keeps its
   bus settings), then initializes the card in a new object. The card object
   is replaced only by the same card. */
static esp_err_t sd_recover(sdmmc_card_t *card)
{
    static sdmmc_card_t fresh;
    int own_slot = card->host.slot;
    sd_host_slot_handle_t handle = sdmmc_get_slot_handle(own_slot);
    if (!handle)
        return ESP_ERR_INVALID_STATE;
    sd_host_sdmmc_ctlr_t *ctlr = __containerof(handle, sd_host_sdmmc_slot_t, drv)->ctlr;
    sdmmc_dev_t *dev = ctlr->hal.dev;
    const uint32_t sdio = SDMMC_INTMASK_IO_SLOT0 | SDMMC_INTMASK_IO_SLOT1;
    esp_err_t result = ESP_ERR_TIMEOUT;

    xSemaphoreTake(ctlr->mutex, portMAX_DELAY);
    /* The controller state the timeout left (cmd holds the last command) */
    ESP_LOGW(TAG, "SD before reset: cmd %08" PRIx32 " status %08" PRIx32
            " rintsts %08" PRIx32 " tcbcnt %08" PRIx32 " tbbcnt %08" PRIx32,
            dev->cmd.val, dev->status.val, dev->rintsts.val,
            dev->tcbcnt.val, dev->tbbcnt.val);
    sdmmc_ll_reset_controller(dev);
    sdmmc_ll_reset_dma(dev);
    sdmmc_ll_reset_fifo(dev);
    int64_t end = esp_timer_get_time() + SD_HOST_SDMMC_RESET_TIMEOUT_US;
    bool reset;
    while (!(reset = sdmmc_ll_is_controller_reset_done(dev) &&
            sdmmc_ll_is_dma_reset_done(dev) && sdmmc_ll_is_fifo_reset_done(dev)) &&
            esp_timer_get_time() < end)
        vTaskDelay(1);
    if (reset)
    {
        /* SDIO interrupts of the other slot stay pending. */
        sdmmc_ll_clear_interrupt(dev, ~sdio);
        sdmmc_ll_init_dma(dev);
        xQueueReset(ctlr->event_queue);
        /* The card's slot starts as after its slot init: 1 bit at the
           probing clock until the card init sets them. */
        sd_host_sdmmc_slot_t *own = ctlr->slot[own_slot];
        own->width.width_state = SD_HOST_SLOT_STATE_INIT;
        own->freq.freq_state = SD_HOST_SLOT_STATE_INIT;
        /* The other slot's clock goes to the controller again at its rate. */
        result = ESP_OK;
        for (int i = 0; i < SOC_SDMMC_NUM_SLOTS && result == ESP_OK; i++)
            if (i != own_slot && ctlr->slot[i] &&
                    ctlr->slot[i]->freq.freq_state == SD_HOST_SLOT_STATE_READY)
                result = sd_host_slot_set_card_clk(ctlr->slot[i]);
    }
    xSemaphoreGive(ctlr->mutex);
    if (result != ESP_OK)
        return result;
    sdmmc_host_t host = card->host;
    result = sdmmc_card_init(&host, &fresh);
    if (result != ESP_OK)
        return result;
    if (memcmp(&fresh.cid, &card->cid, sizeof(fresh.cid)) ||
            fresh.csd.capacity != card->csd.capacity ||
            fresh.csd.sector_size != card->csd.sector_size)
        return ESP_ERR_INVALID_RESPONSE;
    *card = fresh;
    return ESP_OK;
}

/* A card read into dst, or a write from src; after a timeout, recovers the
   card once and tries again. A failed recovery or a second timeout leaves the
   card unavailable until a restart. */
static esp_err_t sd_access(void *dst, const void *src, uint32_t sector,
        uint32_t count)
{
    bool write = src != NULL;

    for (int attempt = 0;; attempt++)
    {
        if (s_card_failed)
            return ESP_ERR_INVALID_STATE;
        esp_err_t result = ESP_OK;

        /* One block at a time (CMD24, no stop command) */
        if (write && s_single_writes && count > 1)
            for (uint32_t i = 0; i < count && result == ESP_OK; i++)
                result = sdmmc_write_sectors(s_sd_card, (const uint8_t *)src +
                        i * s_sd_card->csd.sector_size, sector + i, 1);
        else
            result = write ? sdmmc_write_sectors(s_sd_card, src, sector, count) :
                    sdmmc_read_sectors(s_sd_card, dst, sector, count);
        if (result != ESP_ERR_TIMEOUT)
            return result;
        if (attempt)
        {
            /* The controller may hold the timeout's state again. */
            s_card_failed = true;
            ESP_LOGE(TAG, "SD %s timed out again; card unavailable until restart",
                    write ? "write" : "read");
            return result;
        }
        esp_err_t recovered = sd_recover(s_sd_card);
        if (recovered != ESP_OK)
        {
            s_card_failed = true;
            ESP_LOGE(TAG, "SD recovery failed: %s; card unavailable until restart",
                    esp_err_to_name(recovered));
            return result;
        }
        ESP_LOGW(TAG, "SD %s timed out; card initialized again",
                write ? "write" : "read");
        if (write && count > 1 && !s_single_writes)
        {
            s_single_writes = true;
            ESP_LOGW(TAG, "SD writes one block at a time until a restart");
        }
    }
}

static DSTATUS sd_status(BYTE pdrv)
{
    (void)pdrv;
    return s_card_failed ? STA_NOINIT : 0;
}

static DRESULT sd_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    (void)pdrv;
    esp_err_t result = sd_access(buff, NULL, sector, count);

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "SD read failed: %s sector=%lu count=%u",
                esp_err_to_name(result), (unsigned long)sector, count);
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT sd_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    (void)pdrv;
    esp_err_t result = sd_access(NULL, buff, sector, count);

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "SD write failed: %s sector=%lu count=%u",
                esp_err_to_name(result), (unsigned long)sector, count);
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT sd_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    switch (cmd)
    {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(DWORD *)buff = s_sd_card->csd.capacity;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = s_sd_card->csd.sector_size;
        return RES_OK;
    }
    return RES_ERROR;
}

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

    /* Replaces the IDF disk functions of this drive; FatFs's volume lock
       keeps one access at a time. */
    static const ff_diskio_impl_t impl = {
        .init = sd_status,
        .status = sd_status,
        .read = sd_read,
        .write = sd_write,
        .ioctl = sd_ioctl,
    };
    s_card_failed = false;
    ff_diskio_register(ff_diskio_get_pdrv_card(s_sd_card), &impl);
    ESP_LOGI(TAG, "SD card mounted: %s%s", s_sd_card->cid.name,
            s_single_writes ? ", single-block writes" : "");
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
