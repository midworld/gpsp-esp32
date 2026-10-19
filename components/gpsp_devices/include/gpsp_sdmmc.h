/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_SDMMC_H
#define GPSP_SDMMC_H
#include <stdbool.h>
#include <stdint.h>
bool gpsp_sdmmc_mount(const char *path);
void gpsp_sdmmc_unmount(void);
bool gpsp_sdmmc_mounted(void);
uint8_t gpsp_sdmmc_drive(void);
#endif
