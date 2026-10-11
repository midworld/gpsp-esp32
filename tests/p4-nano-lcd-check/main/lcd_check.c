/* ST7789 transfer-rate check for ESP32-P4-NANO-WIFI6-DB.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "lcd_test_pattern.h"

#define LCD_HOST SPI2_HOST
#define LCD_PIN_SCLK GPIO_NUM_20
#define LCD_PIN_MOSI GPIO_NUM_21
#define LCD_PIN_CS GPIO_NUM_5
#define LCD_PIN_DC GPIO_NUM_26
#define LCD_PIN_RESET GPIO_NUM_25
#define LCD_PIN_BL GPIO_NUM_32
#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define LCD_OFFSET_X ((LCD_WIDTH - LCD_TEST_WIDTH) / 2)
#define LCD_OFFSET_Y ((LCD_HEIGHT - LCD_TEST_HEIGHT) / 2)

struct lcd_test_transfer
{
    SemaphoreHandle_t done;
    /* Read only after taking done, before another transfer is submitted. */
    volatile int64_t completed_at_us;
};

static const char *TAG = "lcd_check";

static bool lcd_test_transfer_done(esp_lcd_panel_io_handle_t io,
        esp_lcd_panel_io_event_data_t *event, void *user_ctx)
{
    struct lcd_test_transfer *transfer = user_ctx;
    BaseType_t woken = pdFALSE;

    (void)io;
    (void)event;
    transfer->completed_at_us = esp_timer_get_time();
    xSemaphoreGiveFromISR(transfer->done, &woken);

    /* The SPI panel IO does not consume the callback's return value. */
    if (woken == pdTRUE)
        portYIELD_FROM_ISR();
    return woken == pdTRUE;
}

static int64_t lcd_test_wait(struct lcd_test_transfer *transfer)
{
    if (xSemaphoreTake(transfer->done, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "LCD DMA completion timed out");
        ESP_ERROR_CHECK(ESP_ERR_TIMEOUT);
    }

    return transfer->completed_at_us;
}

static esp_lcd_panel_handle_t lcd_test_panel_open(unsigned clock_mhz,
        struct lcd_test_transfer *transfer,
        esp_lcd_panel_io_handle_t *io)
{
    esp_lcd_panel_handle_t panel;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = clock_mhz * 1000000u,
        .trans_queue_depth = 2,
        .on_color_trans_done = lcd_test_transfer_done,
        .user_ctx = transfer,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RESET,
#if CONFIG_LCD_TEST_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };

    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BL, 0));
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, io));
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(*io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
#if CONFIG_LCD_TEST_ROTATE_180
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, true));
#else
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
#endif
#if CONFIG_LCD_TEST_INVERT_COLORS
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
#else
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
#endif

    return panel;
}

static void lcd_test_clear(esp_lcd_panel_handle_t panel,
        struct lcd_test_transfer *transfer, uint16_t *pixels)
{
    int y;

    /* A 240x160 buffer also holds a 320x120 clearing strip. */
    memset(pixels, 0, LCD_TEST_BYTES);
    for (y = 0; y < LCD_HEIGHT; y += 120)
    {
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
                panel, 0, y, LCD_WIDTH, y + 120, pixels));
        lcd_test_wait(transfer);
    }
}

static void lcd_test_submit(esp_lcd_panel_handle_t panel, uint16_t *pixels)
{
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel,
            LCD_OFFSET_X, LCD_OFFSET_Y,
            LCD_OFFSET_X + LCD_TEST_WIDTH, LCD_OFFSET_Y + LCD_TEST_HEIGHT,
            pixels));
}

static void lcd_test_run(unsigned clock_mhz,
        struct lcd_test_transfer *transfer, uint16_t *buffers[2])
{
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel = lcd_test_panel_open(clock_mhz, transfer, &io);
    uint32_t frame_number = 0;
    uint32_t completed = 0;
    uint32_t window_frames = 0;
    uint32_t latency_min_us = UINT32_MAX;
    uint32_t latency_max_us = 0;
    uint64_t latency_sum_us = 0;
    uint64_t render_sum_us = 0;
    unsigned next_buffer = 1;
    int64_t submitted_at_us;
    int64_t start_us;
    int64_t window_start_us;
    int64_t completed_at_us;

    lcd_test_clear(panel, transfer, buffers[0]);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BL, 1));
    ESP_LOGI(TAG,
            "LCD_RATE mhz=%u area=%ux%u bytes=%u buffers=2 "
            "pixel_limit_fps=%.2f seconds=%u",
            clock_mhz, LCD_TEST_WIDTH, LCD_TEST_HEIGHT,
            (unsigned)LCD_TEST_BYTES,
            clock_mhz * 1000000.0 / (LCD_TEST_BYTES * 8),
            (unsigned)CONFIG_LCD_TEST_SECONDS);

    lcd_test_pattern_draw(buffers[0], clock_mhz, frame_number++);
    start_us = esp_timer_get_time();
    window_start_us = start_us;
    submitted_at_us = start_us;
    lcd_test_submit(panel, buffers[0]);

    for (;;)
    {
        int64_t render_start_us = esp_timer_get_time();
        uint32_t latency_us;

        /* Prepare the other buffer while DMA owns the submitted buffer. */
        lcd_test_pattern_draw(buffers[next_buffer], clock_mhz, frame_number++);
        render_sum_us += (uint64_t)(esp_timer_get_time() - render_start_us);
        completed_at_us = lcd_test_wait(transfer);
        completed++;
        window_frames++;
        latency_us = (uint32_t)(completed_at_us - submitted_at_us);
        latency_sum_us += latency_us;
        if (latency_us < latency_min_us)
            latency_min_us = latency_us;
        if (latency_us > latency_max_us)
            latency_max_us = latency_us;

        if (completed_at_us - window_start_us >= 1000000)
        {
            ESP_LOGI(TAG, "LCD_FPS mhz=%u fps=%.2f completed=%" PRIu32,
                    clock_mhz,
                    window_frames * 1000000.0 / (completed_at_us - window_start_us),
                    completed);
            window_frames = 0;
            window_start_us = completed_at_us;
        }

        if (completed_at_us - start_us >=
                (int64_t)CONFIG_LCD_TEST_SECONDS * 1000000)
            break;

        submitted_at_us = esp_timer_get_time();
        lcd_test_submit(panel, buffers[next_buffer]);
        next_buffer ^= 1u;
    }

    double fps = completed * 1000000.0 / (completed_at_us - start_us);
    ESP_LOGI(TAG,
            "LCD_RESULT mhz=%u frames=%" PRIu32 " fps=%.2f reaches_60=%s "
            "submit_to_done_us=%" PRIu64 "/%" PRIu32 "/%" PRIu32
            " render_avg_us=%" PRIu64,
            clock_mhz, completed, fps, fps >= 60.0 ? "yes" : "no",
            latency_sum_us / completed, latency_min_us, latency_max_us,
            render_sum_us / completed);

    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BL, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_del(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_io_del(io));
}

void app_main(void)
{
    static const unsigned clocks_mhz[] = {10, 20, 40};
    uint16_t *buffers[2];
    struct lcd_test_transfer transfer = {0};
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
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = LCD_TEST_BYTES,
    };
    unsigned index;

    ESP_LOGI(TAG, "ST7789: SCLK=20 MOSI=21 CS=5 DC=26 RST=25 BL=32");
    ESP_LOGI(TAG, "Landscape 320x240; centered 240x160 at (40,40)");
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BL, 0));
    ESP_ERROR_CHECK(gpio_config(&backlight_config));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

    transfer.done = xSemaphoreCreateBinary();
    if (!transfer.done)
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    for (index = 0; index < 2; index++)
    {
        buffers[index] = heap_caps_malloc(LCD_TEST_BYTES,
                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!buffers[index])
            ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    ESP_LOGI(TAG, "Two DMA buffers: %u bytes; free internal SRAM: %u bytes",
            (unsigned)(LCD_TEST_BYTES * 2),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    for (;;)
        for (index = 0; index < sizeof(clocks_mhz) / sizeof(clocks_mhz[0]); index++)
            lcd_test_run(clocks_mhz[index], &transfer, buffers);
}
