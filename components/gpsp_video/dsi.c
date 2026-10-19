/* DSI scanout, PPA rotation and display effects.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_lcd_dsi.h"
#include "sdkconfig.h"

#if CONFIG_GPSP_LCD && CONFIG_GPSP_LCD_DSI

#include <string.h>
#include <stdatomic.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "panels/dsi_panel.h"

#define DSI_WIDTH 480
#define DSI_HEIGHT 800
#define DSI_BYTES (DSI_WIDTH * DSI_HEIGHT * sizeof(uint16_t))
#define DSI_BUFFERS 3
#define DSI_WORK_BYTES (320 * 240 * sizeof(uint16_t))

static const char *TAG = "gpsp_dsi";
static esp_lcd_dsi_bus_handle_t s_dsi;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static esp_ldo_channel_handle_t s_power;
/* Separate PPA clients for game and UI transactions. */
static ppa_client_handle_t s_game_ppa;
static ppa_client_handle_t s_ui_ppa;
static SemaphoreHandle_t s_frame_done;
static void *s_frames[DSI_BUFFERS];
static unsigned s_widths[DSI_BUFFERS];
static unsigned s_next;
static bool s_ready;
static uint16_t *s_work;
static uint16_t *s_color_lut;
/* One snapshot per frame; UI changes take effect on the next game frame. */
static atomic_uint s_options;
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
static struct
{
    int64_t start_us, render_max_us, present_max_us;
    unsigned game_frames, ui_frames, errors;
} s_diag;

static void dsi_diagnostics(bool ui, unsigned options, int64_t start,
        int64_t rendered, esp_err_t result)
{
    int64_t now = esp_timer_get_time();

    if (!s_diag.start_us)
        s_diag.start_us = start;
    if (rendered - start > s_diag.render_max_us)
        s_diag.render_max_us = rendered - start;
    if (now - rendered > s_diag.present_max_us)
        s_diag.present_max_us = now - rendered;
    if (result != ESP_OK)
        s_diag.errors++;
    else if (ui)
        s_diag.ui_frames++;
    else
        s_diag.game_frames++;
    if (now - s_diag.start_us < 5000000)
        return;

    ESP_LOGI(TAG, "DSI health period_ms=%lld game=%u ui=%u "
            "render_max_us=%lld present_max_us=%lld errors=%u "
            "filter=%u color=%u next=%u core=%d",
            (now - s_diag.start_us) / 1000, s_diag.game_frames, s_diag.ui_frames,
            s_diag.render_max_us, s_diag.present_max_us, s_diag.errors,
            options & 0xff, options >> 8, s_next, xPortGetCoreID());
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.start_us = now;
}
#endif

void gpsp_dsi_options(enum gpsp_display_filter *filter,
        enum gpsp_display_color *color)
{
    unsigned options = atomic_load(&s_options);

    *filter = options & 0xff;
    *color = options >> 8;
}

void gpsp_dsi_set_options(enum gpsp_display_filter filter,
        enum gpsp_display_color color)
{
    if ((unsigned)filter < GPSP_DISPLAY_FILTER_COUNT &&
            (unsigned)color < GPSP_DISPLAY_COLOR_COUNT)
        atomic_store(&s_options, (unsigned)filter | ((unsigned)color << 8));
}

static void dsi_delay(unsigned ms)
{
    if (ms)
    {
        TickType_t ticks = pdMS_TO_TICKS(ms);
        vTaskDelay(ticks ? ticks : 1);
    }
}

static bool dsi_frame_done(esp_lcd_panel_handle_t panel,
        esp_lcd_dpi_panel_event_data_t *event, void *arg)
{
    BaseType_t woken = pdFALSE;

    (void)panel;
    (void)event;
    (void)arg;
    xSemaphoreGiveFromISR(s_frame_done, &woken);
    return woken == pdTRUE;
}

/* Three output buffers leave one full refresh before reusing a buffer,
   even when a completion races with draw_bitmap's buffer selection. */
static esp_err_t dsi_present(void)
{
    xSemaphoreTake(s_frame_done, 0);
    esp_err_t result = esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
            DSI_WIDTH, DSI_HEIGHT, s_frames[s_next]);

    if (result == ESP_OK &&
            !xSemaphoreTake(s_frame_done, pdMS_TO_TICKS(250)))
        result = ESP_ERR_TIMEOUT;
    if (result != ESP_OK)
    {
        /* Do not reuse a buffer whose scanout state is unknown. */
        s_ready = false;
        ESP_LOGE(TAG, "Frame submission failed: %s buffer=%u",
                esp_err_to_name(result), s_next);
        return result;
    }
    s_next = (s_next + 1) % DSI_BUFFERS;
    return ESP_OK;
}

esp_err_t gpsp_dsi_draw(const uint16_t *pixels, bool ui)
{
    if (!s_ready || !pixels)
        return ESP_ERR_INVALID_STATE;
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    int64_t start = esp_timer_get_time();
#endif

    unsigned width = ui ? 320 : 240;
    unsigned height = ui ? 240 : 160;
    unsigned scale = ui ? 2 : 3;
    enum gpsp_display_filter filter;
    enum gpsp_display_color color;

    gpsp_dsi_options(&filter, &color);
    const uint16_t *lut = !ui && color == GPSP_DISPLAY_COLOR_GBA ?
            s_color_lut : NULL;
    bool bilinear = !ui && filter == GPSP_DISPLAY_BILINEAR;
    unsigned offset_y = (DSI_HEIGHT - width * scale) / 2;

    if (bilinear && lut)
    {
        for (unsigned i = 0; i < width * height; i++)
            s_work[i] = lut[pixels[i]];
        pixels = s_work;
    }
    ppa_srm_oper_config_t operation = {
        .in = {
            .buffer = pixels,
            .pic_w = width,
            .pic_h = height,
            .block_w = width,
            .block_h = height,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = bilinear ? s_frames[s_next] : s_work,
            .buffer_size = bilinear ? DSI_BYTES : DSI_WORK_BYTES,
            .pic_w = bilinear ? DSI_WIDTH : height,
            .pic_h = bilinear ? DSI_HEIGHT : width,
            .block_offset_y = bilinear ? offset_y : 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
#if CONFIG_GPSP_LCD_ROTATE_180
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
#else
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
#endif
        .scale_x = bilinear ? scale : 1,
        .scale_y = bilinear ? scale : 1,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    if (s_widths[s_next] != width)
    {
        /* Game and UI borders differ; clear each buffer on a transition. */
        memset(s_frames[s_next], 0, DSI_BYTES);
        esp_err_t result = esp_cache_msync(s_frames[s_next], DSI_BYTES,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        if (result != ESP_OK)
        {
            ESP_LOGE(TAG, "Buffer clear sync failed: %s buffer=%u",
                    esp_err_to_name(result), s_next);
            return result;
        }
        s_widths[s_next] = width;
    }
    esp_err_t result = ppa_do_scale_rotate_mirror(
            ui ? s_ui_ppa : s_game_ppa, &operation);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "PPA failed: %s buffer=%u ui=%d filter=%u color=%u",
                esp_err_to_name(result), s_next, ui, (unsigned)filter,
                (unsigned)color);
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
        dsi_diagnostics(ui, (unsigned)filter | ((unsigned)color << 8),
                start, esp_timer_get_time(), result);
#endif
        return result;
    }
    if (!bilinear)
    {
        int grid_x = -1, grid_y = -1;

        if (!ui && filter == GPSP_DISPLAY_LCD_GRID)
        {
            /* Preserve the logical right/bottom grid edges after rotation. */
#if CONFIG_GPSP_LCD_ROTATE_180
            grid_x = 0;
            grid_y = 2;
#else
            grid_x = 2;
            grid_y = 0;
#endif
        }
        gpsp_display_expand((uint16_t *)s_frames[s_next] + offset_y * DSI_WIDTH,
                s_work, height, width, DSI_WIDTH, scale, grid_x, grid_y, lut);
    }
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    int64_t rendered = esp_timer_get_time();
#endif
    result = dsi_present();
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    dsi_diagnostics(ui, (unsigned)filter | ((unsigned)color << 8),
            start, rendered, result);
#endif
    return result;
}

esp_err_t gpsp_dsi_clear(void)
{
    if (!s_ready)
        return ESP_ERR_INVALID_STATE;
    memset(s_frames[s_next], 0, DSI_BYTES);
    s_widths[s_next] = 0;
    return dsi_present();
}

#define DSI_TRY(call) do { result = (call); \
    if (result != ESP_OK) goto fail; } while (0)

esp_err_t gpsp_dsi_init(void)
{
    const esp_ldo_channel_config_t power = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    const esp_lcd_dsi_bus_config_t bus = {
        .bus_id = 0,
        .num_data_lanes = 2,
        /* IDF selects the PHY reference clock for the chip revision. */
        .phy_clk_src = 0,
        .lane_bit_rate_mbps = gpsp_dsi_panel.lane_mbps,
    };
    const esp_lcd_dbi_io_config_t io = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    const esp_lcd_dpi_panel_config_t panel = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = gpsp_dsi_panel.clock_mhz,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = DSI_BUFFERS,
        .video_timing = gpsp_dsi_panel.timing,
    };
    const ppa_client_config_t ppa = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {
        .on_frame_buf_complete = dsi_frame_done,
    };
    esp_err_t result;

    if (s_panel)
        return ESP_ERR_INVALID_STATE;
    s_frame_done = xSemaphoreCreateBinary();
    if (!s_frame_done)
        return ESP_ERR_NO_MEM;
    s_work = heap_caps_aligned_alloc(64, DSI_WORK_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s_color_lut = heap_caps_malloc(GPSP_DISPLAY_LUT_SIZE * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_work || !s_color_lut)
    {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }
    gpsp_display_gba_lut(s_color_lut);
    DSI_TRY(gpsp_dsi_panel.power());
    DSI_TRY(esp_ldo_acquire_channel(&power, &s_power));
    DSI_TRY(esp_lcd_new_dsi_bus(&bus, &s_dsi));
    DSI_TRY(esp_lcd_new_panel_io_dbi(s_dsi, &io, &s_io));
    for (size_t i = 0; i < gpsp_dsi_panel.command_count; i++)
    {
        const struct dsi_init_command *command = &gpsp_dsi_panel.commands[i];
        DSI_TRY(esp_lcd_panel_io_tx_param(s_io, command->command,
                command->data, command->size));
        dsi_delay(command->delay_ms);
    }
    DSI_TRY(esp_lcd_new_panel_dpi(s_dsi, &panel, &s_panel));
    DSI_TRY(esp_lcd_dpi_panel_get_frame_buffer(s_panel, DSI_BUFFERS,
            &s_frames[0], &s_frames[1], &s_frames[2]));
    DSI_TRY(esp_lcd_dpi_panel_register_event_callbacks(s_panel,
            &callbacks, NULL));
    DSI_TRY(ppa_register_client(&ppa, &s_game_ppa));
    DSI_TRY(ppa_register_client(&ppa, &s_ui_ppa));
    DSI_TRY(esp_lcd_panel_init(s_panel));
    memset(s_widths, 0, sizeof(s_widths));
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    memset(&s_diag, 0, sizeof(s_diag));
#endif
    /* DPI starts scanning buffer 0. */
    s_next = 1;
    s_ready = true;
    DSI_TRY(gpsp_dsi_clear());
    DSI_TRY(gpsp_dsi_panel.backlight(true));
#if CONFIG_GPSP_LCD_DSI_SIMD
    const char *simd = "on";
#else
    const char *simd = "off";
#endif
    ESP_LOGI(TAG, "%s: 800x480, game 3x, UI 2x, SIMD %s",
            gpsp_dsi_panel.name, simd);
    ESP_LOGI(TAG, "DSI clock=%.1fMHz lanes=2 rate=%uMbps buffers=%u init_core=%d",
            (double)gpsp_dsi_panel.clock_mhz, gpsp_dsi_panel.lane_mbps,
            DSI_BUFFERS, xPortGetCoreID());
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Initialization failed: %s", esp_err_to_name(result));
    gpsp_dsi_shutdown();
    return result;
}

void gpsp_dsi_shutdown(void)
{
    s_ready = false;
    gpsp_dsi_panel.backlight(false);
    if (s_panel)
    {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_game_ppa)
        ppa_unregister_client(s_game_ppa);
    if (s_ui_ppa)
        ppa_unregister_client(s_ui_ppa);
    s_game_ppa = NULL;
    s_ui_ppa = NULL;
    if (s_io)
    {
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
    }
    if (s_dsi)
    {
        esp_lcd_del_dsi_bus(s_dsi);
        s_dsi = NULL;
    }
    if (s_power)
    {
        esp_ldo_release_channel(s_power);
        s_power = NULL;
    }
    gpsp_dsi_panel.shutdown();
    if (s_frame_done)
    {
        vSemaphoreDelete(s_frame_done);
        s_frame_done = NULL;
    }
    memset(s_frames, 0, sizeof(s_frames));
    heap_caps_free(s_work);
    s_work = NULL;
    heap_caps_free(s_color_lut);
    s_color_lut = NULL;
}

#endif
