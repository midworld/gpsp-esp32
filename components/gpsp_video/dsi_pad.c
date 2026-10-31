/* Cached virtual controls in native panel orientation.
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
#include "gpsp_font.h"
#include "gpsp_lcd.h"
#include "gpsp_lcd_dsi.h"
#include "gpsp_virtual_pad.h"
#include "libretro.h"
#include "dsi_pad.h"
#include "dsi_overlay.h"

#define BUTTON(name) (1u << RETRO_DEVICE_ID_JOYPAD_##name)
static atomic_uint s_opacity = 25;
/* One picture per region, in the native orientation; a press changes only
   the fill color of its part. */
static uint32_t *s_tiles[GPSP_PAD_REGIONS];
static uint32_t s_masks[GPSP_PAD_REGIONS];
static const uint32_t s_region_masks[GPSP_PAD_REGIONS] = {
    BUTTON(SELECT), BUTTON(START), GPSP_VIRTUAL_PAD_HOME, BUTTON(L), BUTTON(R),
    BUTTON(UP) | BUTTON(DOWN) | BUTTON(LEFT) | BUTTON(RIGHT), BUTTON(A) | BUTTON(B),
};
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_frames, s_updates, s_blend_max, s_build_max;
static uint64_t s_blend_us, s_build_us;
#endif

static void pad_pixel(unsigned region, unsigned x, unsigned y, uint32_t color)
{
    const struct gpsp_pad_rect *r = &gpsp_pad_rects[region];
#if CONFIG_GPSP_LCD_ROTATE_180
    unsigned px = r->height - 1 - y, py = x;
#else
    unsigned px = y, py = r->width - 1 - x;
#endif
    s_tiles[region][py * r->height + px] = color;
}

static uint32_t pad_color(bool pressed, bool border)
{
    return border ? 0xb0ffffffu : pressed ? 0xa090bbd8u : 0x50000000u;
}

static void pad_text(unsigned region, int cx, int cy, const char *text)
{
    unsigned length = strlen(text);
    int x0 = cx - length * GPSP_FONT_WIDTH;
    int y0 = cy - GPSP_FONT_HEIGHT;
    const struct gpsp_pad_rect *r = &gpsp_pad_rects[region];
    for (unsigned i = 0; i < length; i++)
    {
        const uint8_t *glyph = gpsp_font_glyph(text[i]);
        for (unsigned y = 0; y < GPSP_FONT_HEIGHT * 2; y++)
            for (unsigned x = 0; x < GPSP_FONT_WIDTH * 2; x++)
            {
                int px = x0 + i * GPSP_FONT_WIDTH * 2 + x, py = y0 + y;
                if (px >= 0 && px < r->width && py >= 0 && py < r->height &&
                        (glyph[y / 2] & (1u << (x / 2))))
                    pad_pixel(region, px, py, 0xe8ffffffu);
            }
    }
}

static void pad_build(unsigned region, uint32_t mask)
{
    const struct gpsp_pad_rect *r = &gpsp_pad_rects[region];
    for (unsigned y = 0; y < r->height; y++)
        for (unsigned x = 0; x < r->width; x++)
        {
            uint32_t color = 0;
            if (region == GPSP_PAD_AB)
            {
                const int centers[2][2] = {{174, 58}, {58, 148}};
                const unsigned ids[2] = {BUTTON(A), BUTTON(B)};
                for (unsigned i = 0; i < 2; i++)
                {
                    int dx = x - centers[i][0], dy = y - centers[i][1];
                    unsigned d = dx * dx + dy * dy;
                    if (d < 58 * 58)
                        color = pad_color(mask & ids[i], d >= 56 * 56);
                }
            }
            else if (region == GPSP_PAD_DPAD)
            {
                bool vertical = x >= 72 && x < 144;
                bool horizontal = y >= 72 && y < 144;
                if (vertical || horizontal)
                {
                    bool edge = x < 2 || x >= 214 || y < 2 || y >= 214 ||
                            (!horizontal && (x < 74 || x >= 142)) ||
                            (!vertical && (y < 74 || y >= 142));
                    bool pressed = (x < 72 && (mask & BUTTON(LEFT))) ||
                            (x >= 144 && (mask & BUTTON(RIGHT))) ||
                            (y < 72 && (mask & BUTTON(UP))) ||
                            (y >= 144 && (mask & BUTTON(DOWN)));
                    color = pad_color(pressed, edge);
                }
            }
            else
                color = pad_color(mask != 0, x < 2 || y < 2 ||
                        x >= r->width - 2 || y >= r->height - 2);
            pad_pixel(region, x, y, color);
        }
    static const char *const labels[] = {"SELECT", "START", "MENU", "L", "R"};
    if (region < GPSP_PAD_DPAD)
        pad_text(region, r->width / 2, r->height / 2, labels[region]);
    else if (region == GPSP_PAD_AB)
    {
        pad_text(region, 174, 58, "A");
        pad_text(region, 58, 148, "B");
    }
    else
    {
        pad_text(region, 108, 36, "^");
        pad_text(region, 108, 180, "v");
        pad_text(region, 36, 108, "<");
        pad_text(region, 180, 108, ">");
    }
    s_masks[region] = mask;
}

/* One picture per region */
size_t gpsp_dsi_pad_bytes(void)
{
    size_t bytes = 0;

    for (unsigned i = 0; i < GPSP_PAD_REGIONS; i++)
        bytes += (size_t)gpsp_pad_rects[i].width * gpsp_pad_rects[i].height *
                sizeof(uint32_t);
    return bytes;
}

static esp_err_t pad_prepare(void)
{
    for (unsigned i = 0; i < GPSP_PAD_REGIONS; i++)
    {
        const struct gpsp_pad_rect *r = &gpsp_pad_rects[i];
        s_tiles[i] = heap_caps_aligned_alloc(64, r->width * r->height * sizeof(uint32_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s_tiles[i])
            return ESP_ERR_NO_MEM;
        pad_build(i, 0);
    }
    return ESP_OK;
}

/* The fill color of one part, released to pressed or back; borders and
   text keep theirs. */
static inline void pad_fill(uint32_t *pixel, bool pressed)
{
    if (*pixel == pad_color(!pressed, false))
        *pixel = pad_color(pressed, false);
}

/* Only the arm or face-button half whose state changed. */
static void pad_recolor(unsigned region, bool pressed,
        unsigned x, unsigned y, unsigned width, unsigned height)
{
    const struct gpsp_pad_rect *r = &gpsp_pad_rects[region];
#if CONFIG_GPSP_LCD_ROTATE_180
    unsigned px = r->height - y - height, py = x;
#else
    unsigned px = y, py = r->width - x - width;
#endif
    uint32_t *row = s_tiles[region] + py * r->height + px;

    for (unsigned i = 0; i < width; i++, row += r->height)
        for (unsigned j = 0; j < height; j++)
            pad_fill(&row[j], pressed);
}

static void pad_update(unsigned region, uint32_t mask)
{
    uint32_t changed = s_masks[region] ^ mask;
    const struct gpsp_pad_rect *r = &gpsp_pad_rects[region];

    if (region < GPSP_PAD_DPAD)
    {
        if (changed)
            pad_recolor(region, mask != 0, 0, 0, r->width, r->height);
    }
    else if (region == GPSP_PAD_AB)
    {
        if (changed & BUTTON(A)) pad_recolor(region, mask & BUTTON(A), 116, 0, 116, 206);
        if (changed & BUTTON(B)) pad_recolor(region, mask & BUTTON(B), 0, 0, 116, 206);
    }
    else
    {
        if (changed & BUTTON(UP)) pad_recolor(region, mask & BUTTON(UP), 72, 0, 72, 72);
        if (changed & BUTTON(DOWN)) pad_recolor(region, mask & BUTTON(DOWN), 72, 144, 72, 72);
        if (changed & BUTTON(LEFT)) pad_recolor(region, mask & BUTTON(LEFT), 0, 72, 72, 72);
        if (changed & BUTTON(RIGHT)) pad_recolor(region, mask & BUTTON(RIGHT), 144, 72, 72, 72);
    }
    s_masks[region] = mask;
}

/* Omit the transparent corners of the cross and the gap between A and B. */
static esp_err_t pad_blend(uint16_t *frame, unsigned region, unsigned opacity,
        unsigned first, unsigned last)
{
    static const struct gpsp_pad_rect dpad[] = {
        {72, 0, 72, 216}, {0, 72, 72, 72}, {144, 72, 72, 72},
    };
    static const struct gpsp_pad_rect ab[] = {
        {116, 0, 116, 116}, {0, 90, 116, 116},
    };
    const struct gpsp_pad_rect *r = &gpsp_pad_rects[region];
    const struct gpsp_pad_rect whole = {0, 0, r->width, r->height};
    const struct gpsp_pad_rect *parts = &whole;
    unsigned count = 1;

    if (region == GPSP_PAD_DPAD)
    {
        parts = dpad;
        count = sizeof(dpad) / sizeof(dpad[0]);
    }
    else if (region == GPSP_PAD_AB)
    {
        parts = ab;
        count = sizeof(ab) / sizeof(ab[0]);
    }
    for (unsigned i = 0; i < count; i++)
    {
        const struct gpsp_pad_rect *p = &parts[i];
#if CONFIG_GPSP_LCD_ROTATE_180
        unsigned sx = r->height - p->y - p->height, sy = p->x;
        unsigned x = 480 - r->y - r->height + sx, y = r->x + sy;
#else
        unsigned sx = p->y, sy = r->width - p->x - p->width;
        unsigned x = r->y + sx, y = 800 - r->x - r->width + sy;
#endif
        unsigned top = y < first ? first : y;
        unsigned bottom = y + p->width > last ? last : y + p->width;

        if (top >= bottom)
            continue;
        const struct dsi_overlay_crop crop = {
            s_tiles[region], r->height, r->width, sx, sy + top - y,
        };
        esp_err_t result = gpsp_dsi_blend_overlay_crop(frame, &crop,
                p->height, bottom - top, x, top, opacity);

        if (result != ESP_OK)
            return result;
    }
    return ESP_OK;
}

esp_err_t gpsp_dsi_pad_draw(uint16_t *frame, uint32_t state, unsigned opacity,
        unsigned first, unsigned last)
{
    if (!(state & GPSP_VIRTUAL_PAD_VISIBLE))
        return ESP_OK;
    if (!s_tiles[0])
    {
        esp_err_t result = pad_prepare();
        if (result != ESP_OK)
        {
            gpsp_dsi_pad_shutdown();
            return result;
        }
    }
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    unsigned build_us = 0, updates = 0;
    int64_t blend_us = 0;
#endif
    for (unsigned region = 0; region < GPSP_PAD_REGIONS; region++)
    {
        uint32_t mask = state & s_region_masks[region];
        bool build = s_masks[region] != mask;
        if (build)
        {
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
            int64_t start = esp_timer_get_time();
#endif
            pad_update(region, mask);
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
            build_us += esp_timer_get_time() - start;
            updates++;
#endif
        }
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
        int64_t start = esp_timer_get_time();
#endif
        esp_err_t result = pad_blend(frame, region, opacity, first, last);
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
        blend_us += esp_timer_get_time() - start;
#endif
        if (result != ESP_OK)
            return result;
    }
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    portENTER_CRITICAL(&s_lock);
    s_frames++;
    s_updates += updates;
    s_blend_us += blend_us;
    s_build_us += build_us;
    if (blend_us > s_blend_max) s_blend_max = blend_us;
    if (build_us > s_build_max) s_build_max = build_us;
    portEXIT_CRITICAL(&s_lock);
#endif
    return ESP_OK;
}

unsigned gpsp_dsi_pad_opacity(void)
{
    return atomic_load_explicit(&s_opacity, memory_order_relaxed);
}

void gpsp_dsi_set_pad_opacity(unsigned percent)
{
    if (percent != 25 && percent != 50 && percent != 100)
        return;
    if (atomic_exchange_explicit(&s_opacity, percent, memory_order_relaxed) != percent)
        gpsp_lcd_overlay_changed();
}

void gpsp_dsi_pad_status(void)
{
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    unsigned frames, updates, blend_max, build_max;
    uint64_t blend_us, build_us;
    portENTER_CRITICAL(&s_lock);
    frames = s_frames; updates = s_updates;
    blend_us = s_blend_us; build_us = s_build_us;
    blend_max = s_blend_max; build_max = s_build_max;
    s_frames = s_updates = s_blend_max = s_build_max = 0;
    s_blend_us = s_build_us = 0;
    portEXIT_CRITICAL(&s_lock);
    if (frames)
        ESP_LOGI("gpsp_pad", "PAD profile frames=%u blend_avg_us=%u blend_max_us=%u "
                "cache_updates=%u cache_total_us=%u cache_frame_max_us=%u", frames,
                (unsigned)(blend_us / frames), blend_max, updates,
                (unsigned)build_us, build_max);
#endif
}

void gpsp_dsi_pad_shutdown(void)
{
    for (unsigned i = 0; i < GPSP_PAD_REGIONS; i++)
    {
        heap_caps_free(s_tiles[i]);
        s_tiles[i] = NULL;
    }
}
#endif
