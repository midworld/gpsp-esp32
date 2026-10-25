/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int test_open(const char *, int, ...);
static ssize_t test_read(int, void *, size_t);
static ssize_t test_write(int, const void *, size_t);
static int test_sync(int);
static int test_close(int);
static int test_unlink(const char *);
#define open test_open
#define read test_read
#define write test_write
#define fsync test_sync
#define close test_close
#define unlink test_unlink
#include "main/sd_check.c"
#undef open
#undef read
#undef write
#undef fsync
#undef close
#undef unlink

static const char *s_path;
static bool s_short_read, s_short_write, s_corrupt, s_sync_fail, s_unlink_fail;
static int s_open_fds, s_unmounts;
static esp_err_t s_mount_error, s_unmount_error, s_power_error;
static sdmmc_card_t s_card;

static int test_open(const char *path, int flags, ...)
{
    assert(!strcmp(path, TEST_FILE));
    int fd = open(s_path, flags, 0644);
    if (fd >= 0)
        s_open_fds++;
    return fd;
}

static ssize_t test_read(int fd, void *buf, size_t size)
{
    ssize_t count = read(fd, buf, s_short_read ? size / 2 : size);
    if (s_corrupt && count > 0)
        ((unsigned char *)buf)[0] ^= 1;
    return count;
}

static ssize_t test_write(int fd, const void *buf, size_t size)
{
    return write(fd, buf, s_short_write ? size / 2 : size);
}

static int test_sync(int fd)
{
    return s_sync_fail ? -1 : fsync(fd);
}

static int test_close(int fd)
{
    assert(s_open_fds > 0);
    s_open_fds--;
    return close(fd);
}

static int test_unlink(const char *path)
{
    assert(!strcmp(path, TEST_FILE));
    return s_unlink_fail ? -1 : unlink(s_path);
}

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "test error";
}

int64_t esp_timer_get_time(void)
{
    static int64_t now;
    return now += 1000;
}

void *heap_caps_aligned_alloc(size_t alignment, size_t bytes, unsigned caps)
{
    (void)caps;
    return aligned_alloc(alignment, bytes);
}

void heap_caps_free(void *memory) { free(memory); }

esp_err_t esp_vfs_fat_info(const char *path, uint64_t *total, uint64_t *available)
{
    (void)path;
    *total = *available = TEST_SIZE + MIB;
    return ESP_OK;
}

esp_err_t esp_vfs_fat_sdmmc_mount(const char *path, const sdmmc_host_t *host,
        const sdmmc_slot_config_t *slot, const esp_vfs_fat_mount_config_t *config,
        sdmmc_card_t **card)
{
    (void)path;
    (void)host;
    (void)slot;
    assert(!config->format_if_mount_failed);
    *card = &s_card;
    return s_mount_error;
}

esp_err_t esp_vfs_fat_sdcard_unmount(const char *path, sdmmc_card_t *card)
{
    (void)path;
    assert(card == &s_card);
    s_unmounts++;
    return s_unmount_error;
}

esp_err_t sd_pwr_ctrl_new_on_chip_ldo(const sd_pwr_ctrl_ldo_config_t *config,
        sd_pwr_ctrl_handle_t *power)
{
    (void)config;
    *power = &s_card;
    return ESP_OK;
}

esp_err_t sd_pwr_ctrl_del_on_chip_ldo(sd_pwr_ctrl_handle_t power)
{
    assert(power == &s_card);
    return s_power_error;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    s_path = argv[1];
    uint32_t *buf = malloc(CHUNK);
    double write_mb = 0, read_mb = 0;
    const struct sd_mode mode = {"HS", SDMMC_FREQ_HIGHSPEED, 0, 0};
    assert(buf);

    /* An existing user file must survive a refused test. */
    FILE *file = fopen(s_path, "wb");
    assert(file && fwrite("keep", 1, 4, file) == 4);
    assert(!fclose(file));
    assert(!speed_test(&mode, buf, &write_mb, &read_mb));
    char saved[5] = {0};
    file = fopen(s_path, "rb");
    assert(file && fread(saved, 1, 4, file) == 4 && !strcmp(saved, "keep"));
    assert(!fclose(file) && !unlink(s_path));

    assert(speed_test(&mode, buf, &write_mb, &read_mb));
    assert(write_mb > 0 && read_mb > 0 && access(s_path, F_OK) != 0);
    bool *faults[] = {&s_short_read, &s_short_write, &s_corrupt, &s_sync_fail};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++)
    {
        *faults[i] = true;
        assert(!speed_test(&mode, buf, &write_mb, &read_mb));
        *faults[i] = false;
        assert(!s_open_fds && access(s_path, F_OK) != 0);
    }
    s_unlink_fail = true;
    assert(!speed_test(&mode, buf, &write_mb, &read_mb));
    s_unlink_fail = false;
    assert(!unlink(s_path));

    s_card = (sdmmc_card_t) {.cid = {"test"}, .real_freq_khz = 40000,
        .max_freq_khz = 40000};
    assert(run_mode(&mode, NULL, buf, &write_mb, &read_mb) == 1);
    s_card.max_freq_khz = 20000;
    assert(run_mode(&mode, NULL, buf, &write_mb, &read_mb) == -1);
    const struct sd_mode ddr = {"DDR50", SDMMC_FREQ_DDR50, 1, 1};
    s_card.is_uhs1 = 1;
    s_card.max_freq_khz = SDMMC_FREQ_DDR50;
    assert(run_mode(&ddr, NULL, buf, &write_mb, &read_mb) == -1);
    s_card.is_ddr = 1;
    assert(run_mode(&ddr, NULL, buf, &write_mb, &read_mb) == 1);
    s_unmount_error = ESP_FAIL;
    assert(run_mode(&ddr, NULL, buf, &write_mb, &read_mb) == 0);
    s_unmount_error = ESP_OK;
    s_mount_error = ESP_ERR_NOT_SUPPORTED;
    assert(run_mode(&ddr, NULL, buf, &write_mb, &read_mb) == -1);
    s_mount_error = ESP_FAIL;
    assert(run_mode(&ddr, NULL, buf, &write_mb, &read_mb) == 0);
    s_mount_error = ESP_OK;
    assert(s_unmounts == 5);

    s_card.is_uhs1 = s_card.is_ddr = 0;
    s_card.max_freq_khz = SDMMC_FREQ_SDR104;
    /* UHS-I is skipped; a failed power release must still fail the run. */
    s_power_error = ESP_FAIL;
    assert(!sd_check());
    assert(!s_open_fds && access(s_path, F_OK) != 0);
    free(buf);
    puts("SD check PASS: existing files, I/O failures, data, modes and cleanup");
    return 0;
}
