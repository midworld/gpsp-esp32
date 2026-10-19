/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdkconfig.h"
#if CONFIG_GPSP_LCD_DSI && !CONFIG_GPSP_LCD_DSI_OSPTEK
#include "dsi_panel.h"
#include "gpsp_i2c.h"
#include "waveshare_init.h"
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_control;

static esp_err_t dsi_control(uint8_t reg, uint8_t value)
{
    const uint8_t bytes[] = {reg, value};
    return i2c_master_transmit(s_control, bytes, sizeof(bytes), 100);
}

static esp_err_t panel_power(void)
{
    const i2c_device_config_t control = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x45,
        .scl_speed_hz = 100000,
    };
    esp_err_t result = gpsp_i2c_acquire(&s_i2c);
    if (result == ESP_OK)
        result = i2c_master_bus_add_device(s_i2c, &control, &s_control);
    if (result == ESP_OK)
        result = dsi_control(0x95, 0x11);
    if (result == ESP_OK)
        result = dsi_control(0x95, 0x17);
    if (result == ESP_OK)
        result = dsi_control(0x96, 0);
    return result;
}

static esp_err_t panel_backlight(bool enabled)
{
    return s_control ? dsi_control(0x96, enabled ? 0xff : 0) : ESP_OK;
}

static void panel_shutdown(void)
{
    if (s_control)
    {
        i2c_master_bus_rm_device(s_control);
        s_control = NULL;
    }
    if (s_i2c)
    {
        gpsp_i2c_release();
        s_i2c = NULL;
    }
}

const struct gpsp_dsi_panel gpsp_dsi_panel = {
#if CONFIG_GPSP_LCD_DSI_4_3
    .name = "Waveshare 4.3-DSI-TOUCH-A",
#else
    .name = "Waveshare 4-DSI-TOUCH-A",
#endif
    .lane_mbps = 500,
    .clock_mhz = 30,
    .timing = { .h_size = 480, .v_size = 800,
        .hsync_back_porch = 42, .hsync_pulse_width = 12, .hsync_front_porch = 42,
        .vsync_back_porch = 2, .vsync_pulse_width = 8, .vsync_front_porch = 60 },
    .commands = s_init_commands,
    .command_count = sizeof(s_init_commands) / sizeof(s_init_commands[0]),
    .power = panel_power,
    .backlight = panel_backlight,
    .shutdown = panel_shutdown,
};
#endif
