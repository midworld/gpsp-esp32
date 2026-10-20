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
#if CONFIG_GPSP_LCD_DSI_DMA_EXPAND
#include "esp_async_color_convert.h"
#endif
#include "esp_cache.h"
#include "esp_check.h"
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
#include "dsi_hud.h"
#include "dsi_overlay.h"
#include "dsi_pad.h"
#include "gpsp_touch.h"

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
static ppa_client_handle_t s_overlay_ppa;
static bool s_pad_borders[DSI_BUFFERS];
static SemaphoreHandle_t s_frame_done;
static void *s_frames[DSI_BUFFERS];
static unsigned s_widths[DSI_BUFFERS];
static unsigned s_next;
static bool s_ready;
static uint16_t *s_work;
static uint16_t *s_color_lut;
#if CONFIG_GPSP_LCD_DSI_DMA_EXPAND
#define DSI_TILE_ROWS CONFIG_GPSP_LCD_DSI_TILE_ROWS
#define DSI_TILE_BYTES (DSI_WIDTH * DSI_TILE_ROWS * 3 * sizeof(uint16_t))
static async_color_convert_handle_t s_copy;
static uint16_t *s_tile;
static SemaphoreHandle_t s_copy_done[2];

_Static_assert(DSI_TILE_ROWS == 8 || DSI_TILE_ROWS == 12 || DSI_TILE_ROWS == 16,
        "Unsupported scaling tile size");

static bool dsi_copy_done(async_color_convert_handle_t handle,
        async_color_convert_event_data_t *event, void *arg)
{
    BaseType_t woken = pdFALSE;

    (void)handle;
    (void)event;
    xSemaphoreGiveFromISR(arg, &woken);
    return woken == pdTRUE;
}

static void dsi_copy_wait(unsigned slot, unsigned *copy_us)
{
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    int64_t start = copy_us ? esp_timer_get_time() : 0;
#else
    (void)copy_us;
#endif
    xSemaphoreTake(s_copy_done[slot], portMAX_DELAY);
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    if (copy_us)
        *copy_us += (unsigned)(esp_timer_get_time() - start);
#endif
}

static void dsi_copy_shutdown(void)
{
    if (s_copy)
    {
        esp_err_t result;

        /* A callback on the other core can precede descriptor recycling. */
        do
        {
            result = esp_async_color_convert_uninstall(s_copy);
            if (result == ESP_ERR_INVALID_STATE)
                vTaskDelay(1);
        } while (result == ESP_ERR_INVALID_STATE);
        ESP_ERROR_CHECK(result);
    }
    s_copy = NULL;
    heap_caps_free(s_tile);
    s_tile = NULL;
    for (unsigned i = 0; i < 2; i++)
    {
        if (s_copy_done[i])
            vSemaphoreDelete(s_copy_done[i]);
        s_copy_done[i] = NULL;
    }
}

static void dsi_copy_init(void)
{
    const async_color_convert_config_t config = {
        /* Two tiles plus a descriptor still being recycled by its ISR. */
        .backlog = 3,
        .dma_burst_size = 128,
    };
    esp_err_t result = ESP_ERR_NO_MEM;

    s_tile = heap_caps_aligned_alloc(64, DSI_TILE_BYTES * 2,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    for (unsigned i = 0; i < 2; i++)
        s_copy_done[i] = xSemaphoreCreateBinary();
    if (s_tile && s_copy_done[0] && s_copy_done[1])
        result = esp_async_color_convert_install_dma2d(&config, &s_copy);
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "SRAM scaling unavailable: %s", esp_err_to_name(result));
        dsi_copy_shutdown();
    }
}

static esp_err_t dsi_expand_tiles(uint16_t *frame, int grid_x, int grid_y,
        const uint16_t *lut, unsigned *copy_us, const uint16_t *rotate_source)
{
    bool pending[2] = {false, false};
    esp_err_t result = ESP_OK;

    for (unsigned y = 0; y < 240; y += DSI_TILE_ROWS)
    {
        unsigned rows = 240 - y;
        unsigned slot = (y / DSI_TILE_ROWS) & 1;
        uint16_t *tile = s_tile + slot *
                (DSI_TILE_BYTES / sizeof(*s_tile));

        if (rows > DSI_TILE_ROWS)
            rows = DSI_TILE_ROWS;
        /* Reuse only a completed tile; the other transfer stays queued. */
        if (pending[slot])
        {
            dsi_copy_wait(slot, copy_us);
            pending[slot] = false;
        }
        if (rotate_source)
            gpsp_display_rotate_expand(tile, rotate_source, y, rows,
#if CONFIG_GPSP_LCD_ROTATE_180
                    true,
#else
                    false,
#endif
                    grid_x, grid_y, lut);
        else
            gpsp_display_expand(tile, s_work + y * 160, 160, rows,
                    DSI_WIDTH, 3, grid_x, grid_y, lut);
        /* Limit cache maintenance to this tile, not the whole display. */
        const async_color_convert_request_t request = {
            .src_buffer = tile, .src_stride = DSI_WIDTH,
            .src_height = rows * 3,
            .dst_buffer = frame + y * 3 * DSI_WIDTH,
            .dst_stride = DSI_WIDTH, .dst_height = rows * 3,
            .copy_width = DSI_WIDTH, .copy_height = rows * 3,
            .src_color_format = ESP_COLOR_FOURCC_RGB16,
            .dst_color_format = ESP_COLOR_FOURCC_RGB16,
        };
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
        int64_t start = copy_us ? esp_timer_get_time() : 0;
#else
        (void)copy_us;
#endif
        result = esp_async_color_convert(s_copy, &request,
                dsi_copy_done, s_copy_done[slot]);
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
        if (copy_us)
            *copy_us += (unsigned)(esp_timer_get_time() - start);
#endif
        if (result != ESP_OK)
            break;
        pending[slot] = true;
    }
    /* Complete every transfer before PPA blending or scanout. */
    for (unsigned i = 0; i < 2; i++)
        if (pending[i])
            dsi_copy_wait(i, copy_us);
    return result;
}
#endif
/* One snapshot per frame; UI changes take effect on the next game frame. */
static atomic_uint s_options;
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
static unsigned s_profile_frames;
struct dsi_diagnostics
{
    int64_t start_us, render_max_us, present_max_us;
    unsigned game_frames, ui_frames, errors;
    unsigned options, next;
};
static struct dsi_diagnostics s_diag, s_diag_pending;
static portMUX_TYPE s_diag_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_diag_ready;

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

    s_diag.start_us = now - s_diag.start_us;
    s_diag.options = options;
    s_diag.next = s_next;
    portENTER_CRITICAL(&s_diag_lock);
    s_diag_pending = s_diag;
    s_diag_ready = true;
    portEXIT_CRITICAL(&s_diag_lock);
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.start_us = now;
}
#endif

void gpsp_dsi_status(void)
{
    gpsp_dsi_pad_status();
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    struct dsi_diagnostics diag;
    bool ready;

    portENTER_CRITICAL(&s_diag_lock);
    ready = s_diag_ready;
    diag = s_diag_pending;
    s_diag_ready = false;
    portEXIT_CRITICAL(&s_diag_lock);
    if (ready)
        ESP_LOGI(TAG, "DSI health period_ms=%lld game=%u ui=%u "
                "render_max_us=%lld present_max_us=%lld errors=%u "
                "filter=%u color=%u next=%u",
                diag.start_us / 1000, diag.game_frames, diag.ui_frames,
                diag.render_max_us, diag.present_max_us, diag.errors,
                diag.options & 0xff, diag.options >> 8, diag.next);
#endif
}

esp_err_t gpsp_dsi_blend_overlay(uint16_t *frame, const uint32_t *foreground,
        unsigned width, unsigned height, unsigned x, unsigned y)
{
    const ppa_blend_oper_config_t operation = {
        .in_bg = {
            .buffer = frame,
            .pic_w = DSI_WIDTH, .pic_h = DSI_HEIGHT,
            .block_w = width, .block_h = height,
            .block_offset_x = x, .block_offset_y = y,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .in_fg = {
            .buffer = foreground,
            .pic_w = width, .pic_h = height,
            .block_w = width, .block_h = height,
            .blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888,
        },
        .out = {
            .buffer = frame, .buffer_size = DSI_BYTES,
            .pic_w = DSI_WIDTH, .pic_h = DSI_HEIGHT,
            .block_offset_x = x, .block_offset_y = y,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val = 255,
        .fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    return ppa_do_blend(s_overlay_ppa, &operation);
}

#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
static esp_err_t dsi_overlay_check(void)
{
    uint16_t *frame = s_frames[1];
    uint32_t *foreground = (uint32_t *)s_work;
    const uint32_t colors[] = {0x00ff0000, 0xc0000000, 0xffff0000};
    const unsigned band_width = 26;
    const unsigned band_height = 720;
    const unsigned band_y = 40;

    memset(frame, 0xff, DSI_BYTES);
    for (unsigned i = 0; i < band_width * band_height; i++)
        foreground[i] = colors[i % 3];
    for (unsigned band = 0; band < 2; band++)
    {
        esp_err_t result = gpsp_dsi_blend_overlay(frame, foreground,
                band_width, band_height, band ? DSI_WIDTH - band_width : 0,
                band_y);
        if (result != ESP_OK)
            return result;
    }
    for (unsigned y = 0; y < DSI_HEIGHT; y++)
        for (unsigned x = 0; x < DSI_WIDTH; x++)
        {
            uint16_t pixel = frame[y * DSI_WIDTH + x];
            bool band = (x < band_width || x >= DSI_WIDTH - band_width) &&
                    y >= band_y && y < band_y + band_height;
            unsigned fg_x = x < band_width ? x : x - (DSI_WIDTH - band_width);
            unsigned pattern = band ? ((y - band_y) * band_width + fg_x) % 3 : 0;
            bool good = pixel == 0xffff;

            if (band && pattern == 1)
                good = (pixel >> 11) >= 7 && (pixel >> 11) <= 8 &&
                        ((pixel >> 5) & 63) >= 15 &&
                        ((pixel >> 5) & 63) <= 16 &&
                        (pixel & 31) >= 7 && (pixel & 31) <= 8;
            else if (band && pattern == 2)
                good = pixel == 0xf800;
            if (!good)
            {
                ESP_LOGE(TAG, "Overlay check failed x=%u y=%u rgb565=%04x",
                        x, y, pixel);
                return ESP_FAIL;
            }
        }
    ESP_LOGI(TAG, "PPA overlay alpha and bounds check passed");
    return ESP_OK;
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
    bool cpu_rotate = false;
#if CONFIG_GPSP_LCD_DSI_DMA_EXPAND && CONFIG_GPSP_LCD_DSI_CPU_ROTATE
    cpu_rotate = !ui && !bilinear && s_copy;
#endif
    unsigned offset_y = (DSI_HEIGHT - width * scale) / 2;
    uint32_t pad = gpsp_touch_read();
    enum gpsp_display_stats stats = ui ? GPSP_DISPLAY_STATS_OFF : gpsp_dsi_stats_mode();
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    bool sample = !ui && ((s_profile_frames++ & 15) == 0);
    int64_t ppa_start = 0, ppa_end = 0, expand_end = 0, hud_end = 0;
    unsigned copy_us = 0;
#endif

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
    else if (s_pad_borders[s_next])
    {
        /* Controls reach into the black side borders, outside the next image. */
        unsigned bytes = offset_y * DSI_WIDTH * sizeof(uint16_t);
        uint8_t *frame = (uint8_t *)s_frames[s_next];
        memset(frame, 0, bytes);
        memset(frame + DSI_BYTES - bytes, 0, bytes);
        ESP_RETURN_ON_ERROR(esp_cache_msync(frame, bytes,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M), TAG, "Pad border sync");
        ESP_RETURN_ON_ERROR(esp_cache_msync(frame + DSI_BYTES - bytes, bytes,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M), TAG, "Pad border sync");
    }
    s_pad_borders[s_next] = (pad & GPSP_VIRTUAL_PAD_VISIBLE) != 0;
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    if (sample)
        ppa_start = esp_timer_get_time();
#endif
    esp_err_t result = cpu_rotate ? ESP_OK : ppa_do_scale_rotate_mirror(
            ui ? s_ui_ppa : s_game_ppa, &operation);
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    if (sample)
        ppa_end = esp_timer_get_time();
#endif
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
        uint16_t *frame = (uint16_t *)s_frames[s_next] + offset_y * DSI_WIDTH;
#if CONFIG_GPSP_LCD_DSI_DMA_EXPAND
        if (!ui && s_copy)
        {
            result = dsi_expand_tiles(frame, grid_x, grid_y, lut,
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
                    sample ? &copy_us : NULL,
#else
                    NULL,
#endif
                    cpu_rotate ? pixels : NULL);
            if (result != ESP_OK)
            {
                ESP_LOGW(TAG, "SRAM scaling disabled: %s", esp_err_to_name(result));
                dsi_copy_shutdown();
                if (cpu_rotate)
                    gpsp_display_rotate_expand(frame, pixels, 0, 240,
#if CONFIG_GPSP_LCD_ROTATE_180
                            true,
#else
                            false,
#endif
                            grid_x, grid_y, lut);
                else
                    gpsp_display_expand(frame, s_work, height, width, DSI_WIDTH,
                            scale, grid_x, grid_y, lut);
            }
        }
        else
#endif
        {
            gpsp_display_expand(frame, s_work, height, width, DSI_WIDTH,
                    scale, grid_x, grid_y, lut);
        }
    }
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    if (sample)
        expand_end = esp_timer_get_time();
#endif
    esp_err_t pad_result = gpsp_dsi_pad_draw(s_frames[s_next], pad);
    if (pad_result != ESP_OK)
    {
        gpsp_touch_set_enabled(false);
        ESP_LOGW(TAG, "Virtual pad disabled: %s", esp_err_to_name(pad_result));
    }
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    int64_t pad_end = sample ? esp_timer_get_time() : 0;
#endif
    if (stats != GPSP_DISPLAY_STATS_OFF)
    {
        esp_err_t overlay = gpsp_dsi_hud_draw(s_frames[s_next], stats);

        if (overlay != ESP_OK)
        {
            gpsp_dsi_set_stats(GPSP_DISPLAY_STATS_OFF);
            ESP_LOGW(TAG, "STAT blend disabled: %s", esp_err_to_name(overlay));
        }
    }
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    if (sample)
        hud_end = esp_timer_get_time();
    int64_t rendered = esp_timer_get_time();
#endif
    result = dsi_present();
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    if (sample && result == ESP_OK)
        gpsp_dsi_hud_sample(stats, (unsigned)(ppa_end - ppa_start),
                (unsigned)(expand_end - ppa_end),
                (unsigned)(esp_timer_get_time() - hud_end),
                stats != GPSP_DISPLAY_STATS_OFF ?
                (unsigned)(hud_end - pad_end) : 0, copy_us);
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
    const ppa_client_config_t overlay = {
        .oper_type = PPA_OPERATION_BLEND,
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
    DSI_TRY(ppa_register_client(&overlay, &s_overlay_ppa));
#if CONFIG_GPSP_LCD_DSI_DMA_EXPAND
    dsi_copy_init();
#endif
    DSI_TRY(esp_lcd_panel_init(s_panel));
    memset(s_widths, 0, sizeof(s_widths));
    memset(s_pad_borders, 0, sizeof(s_pad_borders));
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    DSI_TRY(dsi_overlay_check());
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag_ready = false;
    s_profile_frames = 0;
#endif
    gpsp_dsi_hud_init();
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
#if CONFIG_GPSP_LCD_DSI_DMA_EXPAND
    ESP_LOGI(TAG, "SRAM scaling tiles=2 rows=%u bytes=%u DMA2D=%s",
            DSI_TILE_ROWS, (unsigned)DSI_TILE_BYTES * 2,
            s_copy ? "on" : "off");
#if CONFIG_GPSP_LCD_DSI_CPU_ROTATE
    if (s_copy)
        ESP_LOGI(TAG, "Integer rotation: CPU1");
#endif
#endif
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Initialization failed: %s", esp_err_to_name(result));
    gpsp_dsi_shutdown();
    return result;
}

void gpsp_dsi_shutdown(void)
{
    s_ready = false;
    gpsp_dsi_hud_shutdown();
    gpsp_dsi_pad_shutdown();
#if CONFIG_GPSP_LCD_DSI_DMA_EXPAND
    dsi_copy_shutdown();
#endif
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
    if (s_overlay_ppa)
        ppa_unregister_client(s_overlay_ppa);
    s_game_ppa = NULL;
    s_ui_ppa = NULL;
    s_overlay_ppa = NULL;
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
