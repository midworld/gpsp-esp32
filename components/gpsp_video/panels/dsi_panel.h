/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_DSI_PANEL_H
#define GPSP_DSI_PANEL_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
struct dsi_init_command
{
    uint8_t command;
    uint8_t data[48];
    uint8_t size;
    uint16_t delay_ms;
};
struct gpsp_dsi_panel
{
    const char *name;
    unsigned lane_mbps;
    float clock_mhz;
    esp_lcd_video_timing_t timing;
    const struct dsi_init_command *commands;
    size_t command_count;
    esp_err_t (*power)(void);
    esp_err_t (*backlight)(bool enabled);
    void (*shutdown)(void);
};
extern const struct gpsp_dsi_panel gpsp_dsi_panel;
#endif
