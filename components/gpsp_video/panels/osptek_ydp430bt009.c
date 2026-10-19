/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdkconfig.h"
#if CONFIG_GPSP_LCD_DSI && CONFIG_GPSP_LCD_DSI_OSPTEK
#include "dsi_panel.h"
#include "gpsp_hardware.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "osptek_init.h"

static bool s_gpio_ready;

static esp_err_t panel_backlight(bool enabled)
{
    int pin = gpsp_hardware.dsi.backlight;
    return !s_gpio_ready || pin < 0 ? ESP_OK : gpio_set_level(pin, enabled);
}

static esp_err_t panel_power(void)
{
    int reset = gpsp_hardware.dsi.reset;
    int backlight = gpsp_hardware.dsi.backlight;
    gpio_config_t config = { .mode = GPIO_MODE_OUTPUT };
    if (reset >= 0)
        config.pin_bit_mask |= 1ULL << reset;
    if (backlight >= 0)
        config.pin_bit_mask |= 1ULL << backlight;
    if (!config.pin_bit_mask)
        return ESP_OK;
    esp_err_t result = s_gpio_ready ? ESP_OK : gpio_config(&config);
    if (result != ESP_OK)
        return result;
    s_gpio_ready = true;
    result = panel_backlight(false);
    if (result != ESP_OK || reset < 0)
        return result;
    result = gpio_set_level(reset, 0);
    if (result != ESP_OK)
        return result;
    vTaskDelay(pdMS_TO_TICKS(10));
    result = gpio_set_level(reset, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    return result;
}

static void panel_shutdown(void)
{
    /* Keep reset and backlight low between display sessions. */
    if (s_gpio_ready && gpsp_hardware.dsi.reset >= 0)
        gpio_set_level(gpsp_hardware.dsi.reset, 0);
}

const struct gpsp_dsi_panel gpsp_dsi_panel = {
    .name = "OSPTEK YDP430BT009 ST7102",
    .lane_mbps = 820,
    .clock_mhz = 37.8f,
    .timing = { .h_size = 480, .v_size = 800,
        .hsync_back_porch = 40, .hsync_pulse_width = 2, .hsync_front_porch = 40,
        .vsync_back_porch = 10, .vsync_pulse_width = 2, .vsync_front_porch = 310 },
    .commands = s_init_commands,
    .command_count = sizeof(s_init_commands) / sizeof(s_init_commands[0]),
    .power = panel_power,
    .backlight = panel_backlight,
    .shutdown = panel_shutdown,
};
#endif
