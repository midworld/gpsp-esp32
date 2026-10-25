/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_hardware.h"

const struct gpsp_hardware gpsp_hardware = {
    .name = "WT9932P4X-TINY",
    .i2c = {0, 7, 8},
    .audio = {0, -1, -1, -1, -1, -1},
    .spi = {-1, -1, -1, -1, -1, -1, -1},
    .dsi = {27, 26},
    .ethernet = {
        .enabled = false,
    },
    .sd = {0, 4, 40000},
};
