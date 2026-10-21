/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpsp_virtual_pad.h"
#include "dsi_overlay.h"

#define WIDTH 480
#define HEIGHT 800
#define PIXELS (WIDTH * HEIGHT)
static uint32_t *s_tiles[GPSP_PAD_REGIONS];
static unsigned calls;
#include "pad_layout.inc"

static uint16_t blend(uint16_t bg, uint32_t fg, unsigned opacity)
{
    unsigned alpha = (fg >> 24) * opacity / 100;
    unsigned r = ((fg >> 16 & 255) * alpha + (bg >> 11) * 8 * (255 - alpha)) / 255;
    unsigned g = ((fg >> 8 & 255) * alpha + (bg >> 5 & 63) * 4 * (255 - alpha)) / 255;
    unsigned b = ((fg & 255) * alpha + (bg & 31) * 8 * (255 - alpha)) / 255;
    return (r >> 3 << 11) | (g >> 2 << 5) | (b >> 3);
}

esp_err_t gpsp_dsi_blend_overlay_crop(uint16_t *frame,
        const struct dsi_overlay_crop *crop, unsigned width, unsigned height,
        unsigned x, unsigned y, unsigned opacity)
{
    assert(width && height && x + width <= WIDTH && y + height <= HEIGHT);
    assert(crop->x + width <= crop->pitch && crop->y + height <= crop->rows);
    calls++;
    for (unsigned row = 0; row < height; row++)
        for (unsigned col = 0; col < width; col++)
        {
            unsigned dest = (y + row) * WIDTH + x + col;
            unsigned source = (crop->y + row) * crop->pitch + crop->x + col;
            frame[dest] = blend(frame[dest], crop->pixels[source], opacity);
        }
    return ESP_OK;
}
#include "pad_blend.inc"

static bool ink(unsigned region, unsigned x, unsigned y)
{
    if (region == GPSP_PAD_DPAD)
        return (x >= 72 && x < 144) || (y >= 72 && y < 144);
    if (region == GPSP_PAD_AB)
    {
        int dx = (int)x - 174, dy = (int)y - 58;
        int bx = (int)x - 58, by = (int)y - 148;
        return dx * dx + dy * dy < 58 * 58 || bx * bx + by * by < 58 * 58;
    }
    return true;
}

static void setup(void)
{
    for (unsigned i = 0; i < GPSP_PAD_REGIONS; i++)
    {
        const struct gpsp_pad_rect *r = &gpsp_pad_rects[i];
        s_tiles[i] = calloc(r->width * r->height, sizeof(uint32_t));
        assert(s_tiles[i]);
        for (unsigned y = 0; y < r->height; y++)
            for (unsigned x = 0; x < r->width; x++)
            {
#if CONFIG_GPSP_LCD_ROTATE_180
                unsigned px = r->height - 1 - y, py = x;
#else
                unsigned px = y, py = r->width - 1 - x;
#endif
                if (ink(i, x, y))
                    s_tiles[i][py * r->height + px] = 0xa0000000u |
                            ((x * 17 + i * 13) & 255) << 16 |
                            ((y * 11 + i * 7) & 255) << 8 | ((x + y) & 255);
            }
    }
}

static void reference(uint16_t *frame, unsigned region, unsigned first,
        unsigned last, unsigned opacity)
{
    const struct gpsp_pad_rect *r = &gpsp_pad_rects[region];
#if CONFIG_GPSP_LCD_ROTATE_180
    unsigned x = WIDTH - r->y - r->height, y = r->x;
#else
    unsigned x = r->y, y = HEIGHT - r->x - r->width;
#endif
    for (unsigned row = 0; row < r->width; row++)
        for (unsigned col = 0; col < r->height; col++)
            if (y + row >= first && y + row < last)
            {
                unsigned dest = (y + row) * WIDTH + x + col;
                frame[dest] = blend(frame[dest], s_tiles[region][row * r->height + col], opacity);
            }
}

int main(void)
{
    setup();
    uint16_t *expected = malloc(PIXELS * sizeof(uint16_t));
    uint16_t *actual = malloc(PIXELS * sizeof(uint16_t));
    assert(expected && actual);
    const unsigned ranges[][2] = {
        {0, 800}, {40, 760}, {80, 720}, {700, 760},
        {0, 0}, {800, 800}, {40, 41}, {759, 761},
    };
    for (unsigned opacity = 25; opacity <= 100; opacity *= 2)
        for (unsigned range = 0; range < sizeof(ranges) / sizeof(ranges[0]); range++)
        {
            for (unsigned i = 0; i < PIXELS; i++)
                actual[i] = expected[i] = (uint16_t)(i * 7919u);
            calls = 0;
            for (unsigned region = 0; region < GPSP_PAD_REGIONS; region++)
            {
                reference(expected, region, ranges[range][0], ranges[range][1], opacity);
                assert(pad_blend(actual, region, opacity,
                        ranges[range][0], ranges[range][1]) == ESP_OK);
            }
            assert(memcmp(actual, expected, PIXELS * sizeof(uint16_t)) == 0);
            assert(calls <= 10 && (range || calls == 10));
        }
    for (unsigned i = 0; i < GPSP_PAD_REGIONS; i++)
        free(s_tiles[i]);
    free(expected);
    free(actual);
    printf("DSI pad crop PASS: rotation=%u, full/partial rows, source bounds, exact pixels\n",
            CONFIG_GPSP_LCD_ROTATE_180);
    return 0;
}
