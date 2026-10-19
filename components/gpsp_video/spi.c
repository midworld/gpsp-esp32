/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdkconfig.h"
#if CONFIG_GPSP_LCD && !CONFIG_GPSP_LCD_DSI
#include "gpsp_spi.h"
#include "gpsp_hardware.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#define LCD_HOST gpsp_hardware.spi.host
#define LCD_PIN_SCLK gpsp_hardware.spi.sclk
#define LCD_PIN_MOSI gpsp_hardware.spi.mosi
#define LCD_PIN_CS gpsp_hardware.spi.cs
#define LCD_PIN_DC gpsp_hardware.spi.dc
#define LCD_PIN_RESET gpsp_hardware.spi.reset
#define LCD_PIN_BL gpsp_hardware.spi.backlight
#define LCD_FRAME_BYTES (240 * 160 * 2)
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static bool s_bus;
static SemaphoreHandle_t s_done;

static bool gpsp_lcd_transfer_done(esp_lcd_panel_io_handle_t io,
        esp_lcd_panel_io_event_data_t *event, void *arg)
{
    BaseType_t woken = pdFALSE;
    (void)io;
    (void)event;
    (void)arg;
    xSemaphoreGiveFromISR(s_done, &woken);
    if (woken == pdTRUE)
        portYIELD_FROM_ISR();
    return woken == pdTRUE;
}

esp_err_t gpsp_spi_init(void)
{
    if (LCD_HOST < 0 || LCD_PIN_BL < 0)
        return ESP_ERR_NOT_SUPPORTED;

    const gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    const spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_FRAME_BYTES,
    };
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = CONFIG_GPSP_LCD_SPI_MHZ * 1000000u,
        .trans_queue_depth = 2,
        .on_color_trans_done = gpsp_lcd_transfer_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        /* Frames stay in PSRAM; DMA reads them without a bounce buffer. */
        .flags.psram_dma_direct = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RESET,
#if CONFIG_GPSP_LCD_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        /* The panel takes gpSP's little-endian RGB565 as is. */
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    esp_err_t result;
    s_done = xSemaphoreCreateBinary();
    if (!s_done)
        return ESP_ERR_NO_MEM;
    result = gpio_config(&backlight_config);
    if (result == ESP_OK)
        result = gpio_set_level(LCD_PIN_BL, 0);
    if (result == ESP_OK)
        result = spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO);
    s_bus = result == ESP_OK;
    if (result == ESP_OK)
        result = esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &s_io);
    if (result == ESP_OK)
        result = esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel);
    if (result == ESP_OK)
        result = esp_lcd_panel_reset(s_panel);
    if (result == ESP_OK)
        result = esp_lcd_panel_init(s_panel);
    if (result == ESP_OK)
        result = esp_lcd_panel_swap_xy(s_panel, true);
    if (result == ESP_OK)
#if CONFIG_GPSP_LCD_ROTATE_180
        result = esp_lcd_panel_mirror(s_panel, false, true);
#else
        result = esp_lcd_panel_mirror(s_panel, true, false);
#endif
    if (result == ESP_OK)
#if CONFIG_GPSP_LCD_INVERT_COLORS
        result = esp_lcd_panel_invert_color(s_panel, true);
#else
        result = esp_lcd_panel_invert_color(s_panel, false);
#endif
    if (result != ESP_OK)
        gpsp_spi_shutdown();
    return result;
}

esp_err_t gpsp_spi_draw(int x0, int y0, int x1, int y1, const void *pixels)
{
    if (!s_panel)
        return ESP_ERR_INVALID_STATE;
    esp_err_t result = esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, pixels);
    if (result == ESP_OK)
        xSemaphoreTake(s_done, portMAX_DELAY);
    return result;
}

void gpsp_spi_backlight(bool enabled)
{
    esp_lcd_panel_disp_on_off(s_panel, enabled);
    gpio_set_level(LCD_PIN_BL, enabled);
}

void gpsp_spi_shutdown(void)
{
    if (s_panel)
    {
        gpsp_spi_backlight(false);
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_io)
    {
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
    }
    if (s_bus)
    {
        spi_bus_free(LCD_HOST);
        s_bus = false;
    }
    if (s_done)
    {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
}
#endif
