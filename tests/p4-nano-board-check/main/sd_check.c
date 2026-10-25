/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "sd_check.h"

#define MIB (1024U * 1024U)
#define CHUNK (128U * 1024U)
#define MOUNT_POINT "/sdcard"
#define TEST_FILE MOUNT_POINT "/SDTEST.BIN"
#define TEST_SIZE ((size_t)CONFIG_P4_NANO_SD_TEST_MIB * MIB)

struct sd_mode
{
    const char *name;
    int freq_khz;
    int ddr;
    int uhs;
};

/* A card switched to 1.8 V stays there until power is removed, so the
 * single UHS-I mode runs last. */
static const struct sd_mode modes[] = {
    {"DS", SDMMC_FREQ_DEFAULT, 0, 0},
    {"HS", SDMMC_FREQ_HIGHSPEED, 0, 0},
#if defined(CONFIG_P4_NANO_SD_UHS_SDR50)
    {"SDR50", SDMMC_FREQ_SDR50, 0, 1},
#elif defined(CONFIG_P4_NANO_SD_UHS_DDR50)
    {"DDR50", SDMMC_FREQ_DDR50, 1, 1},
#elif defined(CONFIG_P4_NANO_SD_UHS_SDR104)
    {"SDR104", SDMMC_FREQ_SDR104, 0, 1},
#endif
};

static void report(const char *mode, const char *result)
{
    printf("BOARD CHECK SD %-9s %s\n", mode, result);
}

static void fill(uint32_t *buf, size_t pos)
{
    for (size_t i = 0; i < CHUNK / sizeof(*buf); i++)
        buf[i] = (uint32_t)(pos + i * sizeof(*buf)) ^ 0x5aa5c33cU;
}

static size_t count_errors(const uint32_t *buf, size_t pos)
{
    size_t bad = 0;
    for (size_t i = 0; i < CHUNK / sizeof(*buf); i++)
        bad += buf[i] != ((uint32_t)(pos + i * sizeof(*buf)) ^ 0x5aa5c33cU);
    return bad;
}

static double mb_per_s(size_t bytes, int64_t us)
{
    return us > 0 ? (double)bytes / us : 0.0;
}

/* Times only write(), fsync() and read(); pattern work is excluded. */
static int speed_test(const struct sd_mode *mode, uint32_t *buf,
        double *write_mb, double *read_mb)
{
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(MOUNT_POINT, &total, &free_bytes) != ESP_OK ||
            free_bytes < TEST_SIZE + MIB)
    {
        printf("SD %s: needs %u MiB free, has %u MiB\n", mode->name,
                (unsigned)(TEST_SIZE / MIB) + 1, (unsigned)(free_bytes / MIB));
        return 0;
    }

    int fd = open(TEST_FILE, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
    {
        printf("SD %s: cannot create %s: %s\n", mode->name, TEST_FILE,
                strerror(errno));
        return 0;
    }
    int ok = 1;
    int64_t write_us = 0;
    for (size_t pos = 0; ok && pos < TEST_SIZE; pos += CHUNK)
    {
        fill(buf, pos);
        int64_t start = esp_timer_get_time();
        ok = write(fd, buf, CHUNK) == (ssize_t)CHUNK;
        write_us += esp_timer_get_time() - start;
    }
    int64_t start = esp_timer_get_time();
    ok &= fsync(fd) == 0;
    write_us += esp_timer_get_time() - start;
    ok &= close(fd) == 0;
    if (!ok)
    {
        printf("SD %s: write failed\n", mode->name);
        unlink(TEST_FILE);
        return 0;
    }

    fd = open(TEST_FILE, O_RDONLY);
    size_t bad = 0;
    int64_t read_us = 0;
    ok = fd >= 0;
    for (size_t pos = 0; ok && pos < TEST_SIZE; pos += CHUNK)
    {
        start = esp_timer_get_time();
        ok = read(fd, buf, CHUNK) == (ssize_t)CHUNK;
        read_us += esp_timer_get_time() - start;
        if (ok)
            bad += count_errors(buf, pos);
    }
    if (fd >= 0)
        close(fd);
    if (unlink(TEST_FILE) != 0)
        ok = 0;

    *write_mb = mb_per_s(TEST_SIZE, write_us);
    *read_mb = mb_per_s(TEST_SIZE, read_us);
    printf("SD %s: %u MiB file write=%.1f MB/s read=%.1f MB/s errors=%u%s\n",
            mode->name, (unsigned)(TEST_SIZE / MIB), *write_mb, *read_mb,
            (unsigned)bad, ok ? "" : " (read failed)");
    return ok && bad == 0;
}

/* Returns 1 on PASS, 0 on FAIL, -1 when the card lacks the mode. */
static int run_mode(const struct sd_mode *mode, sd_pwr_ctrl_handle_t power,
        uint32_t *buf, double *write_mb, double *read_mb)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = mode->freq_khz;
    if (!mode->ddr)
        host.flags &= ~SDMMC_HOST_FLAG_DDR;
    host.pwr_ctrl_handle = power;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    if (mode->uhs)
        slot.flags |= SDMMC_SLOT_FLAG_UHS1;

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot,
            &mount_config, &card);
    if (err != ESP_OK)
    {
        printf("SD %s: mount %s\n", mode->name, esp_err_to_name(err));
        return mode->uhs && err == ESP_ERR_NOT_SUPPORTED ? -1 : 0;
    }

    printf("SD %s: card=%s %u MiB clock=%d kHz uhs1=%d ddr=%d\n", mode->name,
            card->cid.name,
            (unsigned)((uint64_t)card->csd.capacity * card->csd.sector_size /
                    MIB), card->real_freq_khz, card->is_uhs1, card->is_ddr);
    int result;
    if ((mode->uhs && !card->is_uhs1) ||
            card->max_freq_khz < (uint32_t)mode->freq_khz ||
            card->is_ddr != (unsigned)mode->ddr)
    {
        printf("SD %s: card does not support this mode\n", mode->name);
        result = -1;
    }
    else
    {
        result = speed_test(mode, buf, write_mb, read_mb);
    }
    if (esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card) != ESP_OK)
        result = 0;
    return result;
}

int sd_check(void)
{
    uint32_t *buf = heap_caps_aligned_alloc(64, CHUNK,
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buf == NULL)
    {
        report("", "FAIL");
        return 0;
    }

    sd_pwr_ctrl_handle_t power = NULL;
#if CONFIG_P4_NANO_SD_LDO_CHAN >= 0
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = CONFIG_P4_NANO_SD_LDO_CHAN,
    };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &power);
    if (err != ESP_OK)
    {
        printf("SD power: %s\n", esp_err_to_name(err));
        heap_caps_free(buf);
        report("", "FAIL");
        return 0;
    }
#endif

    int pass = 1;
    const char *fastest = NULL;
    double fastest_write = 0.0;
    double fastest_read = 0.0;
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
    {
        const struct sd_mode *mode = &modes[i];
        if (mode->uhs && power == NULL)
        {
            printf("SD %s: needs the on-chip LDO for 1.8 V\n", mode->name);
            report(mode->name, "SKIP");
            continue;
        }
        double write_mb = 0.0;
        double read_mb = 0.0;
        int result = run_mode(mode, power, buf, &write_mb, &read_mb);
        report(mode->name, result > 0 ? "PASS" : result < 0 ? "SKIP" : "FAIL");
        if (result == 0)
        {
            pass = 0;
            /* Without DS the card is missing or unusable. */
            if (i == 0)
                break;
        }
        else if (result > 0 && read_mb >= fastest_read)
        {
            fastest = mode->name;
            fastest_write = write_mb;
            fastest_read = read_mb;
        }
    }
    if (fastest != NULL)
        printf("SD fastest: %s write=%.1f MB/s read=%.1f MB/s\n", fastest,
                fastest_write, fastest_read);

    if (power != NULL)
        if (sd_pwr_ctrl_del_on_chip_ldo(power) != ESP_OK)
            pass = 0;
    heap_caps_free(buf);
    report("", pass ? "PASS" : "FAIL");
    return pass;
}
