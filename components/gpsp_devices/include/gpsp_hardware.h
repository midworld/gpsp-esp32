/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_HARDWARE_H
#define GPSP_HARDWARE_H

#include <stdbool.h>

struct gpsp_hardware
{
    const char *name;
    struct { int port, sda, scl; } i2c;
    struct { int address, amplifier, mclk, bclk, ws, dout; } audio;
    struct { int host, sclk, mosi, cs, dc, reset, backlight; } spi;
    struct { int reset, backlight; } dsi;
    struct { int address; } touch;
    struct
    {
        bool enabled;
        int phy, reset, clock, mdc, mdio, crs_dv, rxd0, rxd1;
        int txd0, txd1, tx_en;
    } ethernet;
    struct { int slot, width, frequency_khz; } sd;
};

extern const struct gpsp_hardware gpsp_hardware;

#endif
