/* PPA statistics bars using the shared LCD values and layout.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdkconfig.h"

#if CONFIG_GPSP_LCD && CONFIG_GPSP_LCD_DSI

#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "dsi_hud.h"
#include "dsi_overlay.h"
#include "gpsp_font.h"
#include "gpsp_lcd_dsi.h"

#define HUD_SCALE 2
#define HUD_X 40
#define HUD_WIDTH 720
#define HUD_HEIGHT (GPSP_FONT_HEIGHT * HUD_SCALE)
/* FPS/SPD occupy the last 18 columns of the shared top bar. */
#define HUD_SIMPLE_WIDTH (18 * GPSP_FONT_WIDTH * HUD_SCALE)
#define HUD_BAR_PIXELS (HUD_WIDTH * HUD_HEIGHT)
#define HUD_PIXELS (HUD_BAR_PIXELS * 2)
#define HUD_BYTES (HUD_PIXELS * sizeof(uint32_t))
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
#define HUD_FIELDS 5

struct hud_times
{
    unsigned count;
    uint64_t total[HUD_FIELDS];
    unsigned max[HUD_FIELDS];
};
#endif

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t *s_tiles[2];
static unsigned s_active;
static int s_reader = -1;
static atomic_uint s_mode;
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
static struct hud_times s_times[GPSP_DISPLAY_STATS_COUNT];
static unsigned s_build_count, s_build_max;
static uint64_t s_build_total;
static int64_t s_log_us;

/* Logging stays on the status worker, outside the display path. */
static void hud_profile_log(void)
{
    int64_t now = esp_timer_get_time();
    struct hud_times times[GPSP_DISPLAY_STATS_COUNT];

    if (now - s_log_us < 10000000)
        return;
    portENTER_CRITICAL(&s_lock);
    memcpy(times, s_times, sizeof(times));
    memset(s_times, 0, sizeof(s_times));
    portEXIT_CRITICAL(&s_lock);
    for (unsigned mode = 0; mode < GPSP_DISPLAY_STATS_COUNT; mode++)
    {
        const struct hud_times *t = &times[mode];

        if (!t->count)
            continue;
        ESP_LOGI("gpsp_hud", "STAT profile mode=%u samples=%u "
                "ppa_avg_us=%u expand_avg_us=%u present_avg_us=%u "
                "blend_avg_us=%u blend_max_us=%u dma_wait_avg_us=%u", mode, t->count,
                (unsigned)(t->total[0] / t->count),
                (unsigned)(t->total[1] / t->count),
                (unsigned)(t->total[2] / t->count),
                (unsigned)(t->total[3] / t->count), t->max[3],
                (unsigned)(t->total[4] / t->count));
    }
    if (s_build_count)
        ESP_LOGI("gpsp_hud", "STAT cache updates=%u avg_us=%u max_us=%u",
                s_build_count, (unsigned)(s_build_total / s_build_count),
                s_build_max);
    s_build_count = s_build_max = 0;
    s_build_total = 0;
    s_log_us = now;
}
#endif

static uint32_t hud_color(uint16_t color)
{
    unsigned r = color >> 11;
    unsigned g = (color >> 5) & 63;
    unsigned b = color & 31;

    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

/* Cache the text in native panel orientation, ready for PPA blending. */
static void hud_text(uint32_t *tile, const struct lcd_status_position *p,
        const char *text, uint16_t color)
{
    unsigned x0 = p->column * GPSP_FONT_WIDTH * HUD_SCALE;
    uint32_t argb = hud_color(color);

    if (p->right)
        x0 = HUD_WIDTH - x0;
    if (p->bottom)
        tile += HUD_BAR_PIXELS;
    for (unsigned i = 0; text[i] && i < p->chars; i++)
    {
        const uint8_t *glyph = gpsp_font_glyph(text[i]);

        for (unsigned row = 0; row < GPSP_FONT_HEIGHT; row++)
            for (unsigned col = 0; col < GPSP_FONT_WIDTH; col++)
            {
                if (!(glyph[row] & (1u << col)))
                    continue;
                for (unsigned dy = 0; dy < HUD_SCALE; dy++)
                    for (unsigned dx = 0; dx < HUD_SCALE; dx++)
                    {
                        unsigned x = x0 +
                                (i * GPSP_FONT_WIDTH + col) * HUD_SCALE + dx;
                        unsigned y = row * HUD_SCALE + dy;
#if CONFIG_GPSP_LCD_ROTATE_180
                        unsigned px = HUD_HEIGHT - 1 - y;
                        unsigned py = x;
#else
                        unsigned px = y;
                        unsigned py = HUD_WIDTH - 1 - x;
#endif
                        tile[py * HUD_HEIGHT + px] = argb;
                    }
            }
    }
}

static void hud_build(uint32_t *tile, const struct lcd_status_value *values)
{
    for (unsigned band = 0; band < 2; band++)
        for (unsigned row = 0; row < HUD_WIDTH; row++)
        {
            for (unsigned x = 0; x < HUD_HEIGHT; x++)
                tile[band * HUD_BAR_PIXELS + row * HUD_HEIGHT + x] = 0xc0000000u;
        }
    for (unsigned i = 0; i < sizeof(lcd_status_labels) /
            sizeof(lcd_status_labels[0]); i++)
        hud_text(tile, &lcd_status_labels[i].position,
                lcd_status_labels[i].text, LCD_COLOR_LABEL);
    for (unsigned i = 0; i < LCD_FIELD_COUNT; i++)
        hud_text(tile, &lcd_status_positions[i], values[i].text,
                values[i].color);
}

void gpsp_dsi_hud_init(void)
{
    uint32_t *tiles = heap_caps_aligned_alloc(64, HUD_BYTES * 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);

    if (!tiles)
    {
        ESP_LOGW("gpsp_hud", "STAT allocation failed");
        return;
    }
    s_tiles[0] = tiles;
    s_tiles[1] = tiles + HUD_PIXELS;
    s_active = 0;
    s_reader = -1;
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    memset(&s_times, 0, sizeof(s_times));
    s_build_count = s_build_max = 0;
    s_build_total = 0;
    s_log_us = esp_timer_get_time();
#endif
    const struct lcd_status_value empty[LCD_FIELD_COUNT] = {0};

    hud_build(s_tiles[0], empty);
}

enum gpsp_display_stats gpsp_dsi_stats_mode(void)
{
    return atomic_load_explicit(&s_mode, memory_order_relaxed);
}

void gpsp_dsi_set_stats(enum gpsp_display_stats mode)
{
    if ((unsigned)mode < GPSP_DISPLAY_STATS_COUNT)
        atomic_store_explicit(&s_mode, s_tiles[0] ? mode : GPSP_DISPLAY_STATS_OFF,
                memory_order_relaxed);
}

/* Called after the shared LCD formatter by the low-priority status worker. */
void gpsp_dsi_hud_update(const struct lcd_status_value *values)
{
    unsigned next;

#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    hud_profile_log();
#endif
    if (gpsp_dsi_stats_mode() == GPSP_DISPLAY_STATS_OFF)
        return;
    portENTER_CRITICAL(&s_lock);
    next = s_active ^ 1u;
    if (s_reader == (int)next)
    {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    portEXIT_CRITICAL(&s_lock);
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    int64_t start = esp_timer_get_time();
#endif
    hud_build(s_tiles[next], values);
    portENTER_CRITICAL(&s_lock);
    s_active = next;
    portEXIT_CRITICAL(&s_lock);
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    unsigned elapsed = (unsigned)(esp_timer_get_time() - start);

    s_build_count++;
    s_build_total += elapsed;
    if (elapsed > s_build_max)
        s_build_max = elapsed;
#endif
}

esp_err_t gpsp_dsi_hud_draw(uint16_t *frame, enum gpsp_display_stats mode)
{
    unsigned active;
    esp_err_t result = ESP_OK;

    if (mode == GPSP_DISPLAY_STATS_OFF)
        return ESP_OK;
    portENTER_CRITICAL(&s_lock);
    active = s_active;
    s_reader = (int)active;
    portEXIT_CRITICAL(&s_lock);
    unsigned bands = mode == GPSP_DISPLAY_STATS_FULL ? 2 : 1;
    unsigned width = mode == GPSP_DISPLAY_STATS_SIMPLE ? HUD_SIMPLE_WIDTH : HUD_WIDTH;

    for (unsigned band = 0; band < bands && result == ESP_OK; band++)
    {
        const uint32_t *tile = s_tiles[active] + band * HUD_BAR_PIXELS;
        unsigned y = HUD_X;
#if CONFIG_GPSP_LCD_ROTATE_180
        unsigned x = band ? 0 : 480 - HUD_HEIGHT;

        tile += (HUD_WIDTH - width) * HUD_HEIGHT;
        y += HUD_WIDTH - width;
#else
        unsigned x = band ? 480 - HUD_HEIGHT : 0;
#endif
        result = gpsp_dsi_blend_overlay(frame, tile, HUD_HEIGHT, width, x, y);
    }
    portENTER_CRITICAL(&s_lock);
    s_reader = -1;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
void gpsp_dsi_hud_sample(enum gpsp_display_stats mode, unsigned ppa, unsigned expand,
        unsigned present, unsigned blend, unsigned copy)
{
    const unsigned values[HUD_FIELDS] = {ppa, expand, present, blend, copy};
    struct hud_times *times = &s_times[mode];

    portENTER_CRITICAL(&s_lock);
    times->count++;
    for (unsigned i = 0; i < HUD_FIELDS; i++)
    {
        times->total[i] += values[i];
        if (values[i] > times->max[i])
            times->max[i] = values[i];
    }
    portEXIT_CRITICAL(&s_lock);
}
#endif

void gpsp_dsi_hud_shutdown(void)
{
    atomic_store(&s_mode, GPSP_DISPLAY_STATS_OFF);
    heap_caps_free(s_tiles[0]);
    s_tiles[0] = NULL;
    s_tiles[1] = NULL;
}

#endif
