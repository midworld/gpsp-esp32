/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_hardware.h"
#include "driver/spi_master.h"

const struct gpsp_hardware gpsp_hardware = {
    .name = "OSPTEK P4C5",
    .i2c = {0, 7, 8},
    .audio = {0x18, 53, 13, 12, 10, 9},
    .spi = {-1, -1, -1, -1, -1, -1, -1},
    .dsi = {5, 20},
    .touch = {0x55},
    .ethernet = {
        .enabled = false,
    },
    .sd = {0, 4, 40000},
};
