/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_hardware.h"
#include "driver/spi_master.h"

const struct gpsp_hardware gpsp_hardware = {
    .name = "Waveshare P4 Nano",
    .i2c = {0, 7, 8},
    .audio = {0x18, 53, 13, 12, 10, 9},
    .spi = {SPI2_HOST, 20, 21, 5, 26, 25, 32},
    .dsi = {-1, -1},
    .ethernet = {
        .enabled = true, .phy = 1, .reset = 51, .clock = 50,
        .mdc = 31, .mdio = 52, .crs_dv = 28, .rxd0 = 29, .rxd1 = 30,
        .txd0 = 34, .txd1 = 35, .tx_en = 49,
    },
    .sd = {0, 4, 40000},
};
