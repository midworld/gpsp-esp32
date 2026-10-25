#!/usr/bin/env python3
# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SDK = r'''#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_SUPPORTED 0x106
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
#define SDMMC_HOST_SLOT_0 0
#define SDMMC_HOST_FLAG_DDR 1
#define SDMMC_SLOT_FLAG_INTERNAL_PULLUP 1
#define SDMMC_SLOT_FLAG_UHS1 2
#define SDMMC_FREQ_DEFAULT 20000
#define SDMMC_FREQ_HIGHSPEED 40000
#define SDMMC_FREQ_SDR50 100000
#define SDMMC_FREQ_DDR50 50000
#define SDMMC_FREQ_SDR104 200000
typedef void *sd_pwr_ctrl_handle_t;
typedef struct { int ldo_chan_id; } sd_pwr_ctrl_ldo_config_t;
typedef struct {
    int slot, max_freq_khz, flags;
    sd_pwr_ctrl_handle_t pwr_ctrl_handle;
} sdmmc_host_t;
typedef struct { int width, flags; } sdmmc_slot_config_t;
#define SDMMC_HOST_DEFAULT() { .flags = SDMMC_HOST_FLAG_DDR }
#define SDMMC_SLOT_CONFIG_DEFAULT() {0}
typedef struct {
    struct { char name[8]; } cid;
    struct { unsigned capacity, sector_size; } csd;
    int real_freq_khz;
    unsigned max_freq_khz, is_uhs1, is_ddr;
} sdmmc_card_t;
typedef struct { bool format_if_mount_failed; int max_files, allocation_unit_size; }
    esp_vfs_fat_mount_config_t;
const char *esp_err_to_name(esp_err_t);
int64_t esp_timer_get_time(void);
void *heap_caps_aligned_alloc(size_t, size_t, unsigned);
void heap_caps_free(void *);
esp_err_t esp_vfs_fat_info(const char *, uint64_t *, uint64_t *);
esp_err_t esp_vfs_fat_sdmmc_mount(const char *, const sdmmc_host_t *,
        const sdmmc_slot_config_t *, const esp_vfs_fat_mount_config_t *, sdmmc_card_t **);
esp_err_t esp_vfs_fat_sdcard_unmount(const char *, sdmmc_card_t *);
esp_err_t sd_pwr_ctrl_new_on_chip_ldo(const sd_pwr_ctrl_ldo_config_t *, sd_pwr_ctrl_handle_t *);
esp_err_t sd_pwr_ctrl_del_on_chip_ldo(sd_pwr_ctrl_handle_t);
'''

with tempfile.TemporaryDirectory(prefix="gpsp-sd-check-") as directory:
    build = Path(directory)
    (build / "sd_test_sdk.h").write_text(SDK)
    for name in ("driver/sdmmc_host.h", "esp_heap_caps.h", "esp_timer.h",
                 "esp_vfs_fat.h", "sd_pwr_ctrl_by_on_chip_ldo.h", "sdmmc_cmd.h"):
        header = build / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sd_test_sdk.h"\n')
    command = shlex.split(os.environ.get("HOST_CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
        "-fsanitize=address,undefined", "-DCONFIG_P4_NANO_SD_TEST_MIB=1",
        "-DCONFIG_P4_NANO_SD_LDO_CHAN=4", "-DCONFIG_P4_NANO_SD_UHS_SDR104=1",
        "-I", str(build), str(HERE / "test_sd.c"), "-o", str(build / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(build / "test"), str(build / "SDTEST.BIN")], check=True)
