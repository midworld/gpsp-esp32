/* Cached virtual controls in native panel orientation.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdkconfig.h"
#if CONFIG_GPSP_LCD && CONFIG_GPSP_LCD_DSI
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gpsp_font.h"
#include "gpsp_virtual_pad.h"
#include "libretro.h"
#include "dsi_pad.h"
#include "dsi_overlay.h"

#define BUTTON(name) (1u << RETRO_DEVICE_ID_JOYPAD_##name)
static uint32_t *s_tiles[GPSP_PAD_REGIONS];
static uint32_t *s_art[GPSP_PAD_REGIONS][2];
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

static esp_err_t pad_prepare(void)
{
    for (unsigned i = 0; i < GPSP_PAD_REGIONS; i++)
    {
        const struct gpsp_pad_rect *r = &gpsp_pad_rects[i];
        size_t bytes = r->width * r->height * sizeof(uint32_t);
        for (unsigned pressed = 0; pressed < 2; pressed++)
        {
            s_art[i][pressed] = heap_caps_aligned_alloc(64, bytes,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            if (!s_art[i][pressed])
                return ESP_ERR_NO_MEM;
            s_tiles[i] = s_art[i][pressed];
            pad_build(i, pressed ? s_region_masks[i] : 0);
        }
        if (i >= GPSP_PAD_DPAD)
        {
            s_tiles[i] = heap_caps_aligned_alloc(64, bytes,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            if (!s_tiles[i])
                return ESP_ERR_NO_MEM;
            memcpy(s_tiles[i], s_art[i][0], bytes);
        }
        else
            s_tiles[i] = s_art[i][0];
        s_masks[i] = 0;
    }
    return ESP_OK;
}

/* Copy only the arm or face-button half whose state changed. */
static void pad_copy(unsigned region, bool pressed,
        unsigned x, unsigned y, unsigned width, unsigned height)
{
    const struct gpsp_pad_rect *r = &gpsp_pad_rects[region];
#if CONFIG_GPSP_LCD_ROTATE_180
    unsigned px = r->height - y - height, py = x;
#else
    unsigned px = y, py = r->width - x - width;
#endif
    unsigned offset = py * r->height + px;
    const uint32_t *source = s_art[region][pressed] + offset;
    uint32_t *dest = s_tiles[region] + offset;
    if (height == r->height)
        memcpy(dest, source, width * height * sizeof(uint32_t));
    else
        for (unsigned row = 0; row < width; row++)
            memcpy(dest + row * r->height, source + row * r->height,
                    height * sizeof(uint32_t));
}

static void pad_update(unsigned region, uint32_t mask)
{
    uint32_t changed = s_masks[region] ^ mask;
    if (region < GPSP_PAD_DPAD)
        s_tiles[region] = s_art[region][mask != 0];
    else if (region == GPSP_PAD_AB)
    {
        if (changed & BUTTON(A)) pad_copy(region, mask & BUTTON(A), 116, 0, 116, 206);
        if (changed & BUTTON(B)) pad_copy(region, mask & BUTTON(B), 0, 0, 116, 206);
    }
    else
    {
        if (changed & BUTTON(UP)) pad_copy(region, mask & BUTTON(UP), 72, 0, 72, 72);
        if (changed & BUTTON(DOWN)) pad_copy(region, mask & BUTTON(DOWN), 72, 144, 72, 72);
        if (changed & BUTTON(LEFT)) pad_copy(region, mask & BUTTON(LEFT), 0, 72, 72, 72);
        if (changed & BUTTON(RIGHT)) pad_copy(region, mask & BUTTON(RIGHT), 144, 72, 72, 72);
    }
    s_masks[region] = mask;
}

esp_err_t gpsp_dsi_pad_draw(uint16_t *frame, uint32_t state)
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
        const struct gpsp_pad_rect *r = &gpsp_pad_rects[region];
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
#if CONFIG_GPSP_LCD_ROTATE_180
        unsigned x = 480 - r->y - r->height, y = r->x;
#else
        unsigned x = r->y, y = 800 - r->x - r->width;
#endif
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
        int64_t start = esp_timer_get_time();
#endif
        esp_err_t result = gpsp_dsi_blend_overlay(frame, s_tiles[region],
                r->height, r->width, x, y);
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
        if (i >= GPSP_PAD_DPAD && s_tiles[i] != s_art[i][0] &&
                s_tiles[i] != s_art[i][1])
            heap_caps_free(s_tiles[i]);
        heap_caps_free(s_art[i][0]);
        heap_caps_free(s_art[i][1]);
        s_art[i][0] = s_art[i][1] = NULL;
        s_tiles[i] = NULL;
    }
}
#endif
