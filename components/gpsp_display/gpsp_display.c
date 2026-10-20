/* RGB565 display effects.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_display.h"

#include <assert.h>
#include <math.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if CONFIG_GPSP_LCD_DSI_SIMD
void gpsp_display_nearest2_p4(uint16_t *out, const uint16_t *in,
        unsigned groups, unsigned pitch_bytes);
void gpsp_display_nearest3_p4(uint16_t *out, const uint16_t *in,
        unsigned groups, unsigned pitch_bytes);
void gpsp_display_grid3_p4(uint16_t *bright0, uint16_t *bright1, uint16_t *dark,
        const uint16_t *normal,
        unsigned groups, bool edge_first);
void gpsp_display_transpose4_p4(uint16_t *output, const uint16_t *input,
        int input_pitch_bytes, int output_pitch_bytes);
#endif

/* GBA sRGB matrix/gamma: Pokefan531, hunterk, crashGG (public domain).
   https://github.com/libretro/slang-shaders/blob/master/handheld/shaders/color/gba-color.slang */
static unsigned display_linear_index(float value)
{
    if (value <= 0)
        return 0;
    if (value >= 1)
        return 1023;
    return (unsigned)(value * 1023 + 0.5f);
}

void gpsp_display_gba_lut(uint16_t *lut)
{
    float linear5[32], linear6[64];
    uint16_t encoded[1024];

    for (unsigned i = 0; i < 32; i++)
        linear5[i] = powf(i / 31.0f, 2.2f);
    for (unsigned i = 0; i < 64; i++)
        linear6[i] = powf(i / 63.0f, 2.2f);
    for (unsigned i = 0; i < 1024; i++)
    {
        float value = powf(i / 1023.0f, 1.0f / 2.2f);
        unsigned v5 = (unsigned)(value * 31 + 0.5f);
        unsigned v6 = (unsigned)(value * 63 + 0.5f);

        encoded[i] = (v5 << 11) | (v6 << 5) | v5;
    }
    for (unsigned i = 0; i < GPSP_DISPLAY_LUT_SIZE; i++)
    {
        float r = linear5[i >> 11];
        float g = linear6[(i >> 5) & 63];
        float b = linear5[i & 31];
        unsigned red = display_linear_index((0.905f * r + 0.195f * g -
                0.10f * b) * 0.91f);
        unsigned green = display_linear_index((0.10f * r + 0.65f * g +
                0.25f * b) * 0.91f);
        unsigned blue = display_linear_index((0.1575f * r + 0.1425f * g +
                0.70f * b) * 0.91f);

        lut[i] = (encoded[red] & 0xf800) | (encoded[green] & 0x07e0) |
                (encoded[blue] & 0x001f);
    }
}

static uint16_t display_dim(uint16_t pixel)
{
    unsigned r = ((pixel >> 11) * 3 + 2) / 4;
    unsigned g = (((pixel >> 5) & 63) * 3 + 2) / 4;
    unsigned b = ((pixel & 31) * 3 + 2) / 4;

    return (r << 11) | (g << 5) | b;
}

void gpsp_display_expand(uint16_t *output, const uint16_t *input,
        unsigned width, unsigned height, unsigned pitch, unsigned scale,
        int grid_x, int grid_y, const uint16_t *lut)
{
    uint16_t colors[240] __attribute__((aligned(16)));
    bool grid = grid_x >= 0;

    assert(width <= 240 && (scale == 2 || scale == 3));
    assert(pitch >= width * scale);
    assert((grid && scale == 3 && (grid_x == 0 || grid_x == 2) &&
            (grid_y == 0 || grid_y == 2)) || (!grid && grid_y == -1));

    for (unsigned y = 0; y < height; y++)
    {
        uint16_t *row = output + y * scale * pitch;
        const uint16_t *source = input + y * width;
        const uint16_t *normal = source;

        if (lut)
        {
            for (unsigned x = 0; x < width; x++)
                colors[x] = lut[source[x]];
            normal = colors;
        }
        if (grid)
        {
            uint16_t *bright0 = grid_y == 0 ? row + pitch : row;
            uint16_t *bright1 = bright0 + pitch;
            uint16_t *dark = row + grid_y * pitch;

#if CONFIG_GPSP_LCD_DSI_SIMD
            if (!(width % 8) && !(((uintptr_t)row | (uintptr_t)normal) % 16) &&
                    !(pitch % 8))
            {
                gpsp_display_grid3_p4(bright0, bright1, dark, normal,
                        width / 8, grid_x == 0);
                continue;
            }
#endif
            for (unsigned x = 0; x < width; x++)
            {
                uint16_t pixel = normal[x];
                uint16_t dim = display_dim(pixel);
                uint16_t corner = display_dim(dim);

                for (unsigned dx = 0; dx < 3; dx++)
                {
                    bool edge = dx == (unsigned)grid_x;

                    bright0[x * 3 + dx] = edge ? dim : pixel;
                    dark[x * 3 + dx] = edge ? corner : dim;
                }
            }
            memcpy(bright1, bright0, width * 3 * sizeof(*row));
            continue;
        }
#if CONFIG_GPSP_LCD_DSI_SIMD
        if (!(width % 8) && !(((uintptr_t)row | (uintptr_t)normal) % 16) &&
                !(pitch % 8))
        {
            if (scale == 2)
                gpsp_display_nearest2_p4(row, normal, width / 8, pitch * 2);
            else
                gpsp_display_nearest3_p4(row, normal, width / 8, pitch * 2);
            continue;
        }
#endif
        for (unsigned x = 0; x < width; x++)
            for (unsigned dx = 0; dx < scale; dx++)
                row[x * scale + dx] = normal[x];
        for (unsigned dy = 1; dy < scale; dy++)
            memcpy(row + dy * pitch, row, width * scale * sizeof(*row));
    }
}

void gpsp_display_rotate_expand(uint16_t *output, const uint16_t *input,
        unsigned first, unsigned rows, bool rotate_180,
        int grid_x, int grid_y, const uint16_t *lut)
{
    assert(rows && !(first % 4) && !(rows % 4) && first + rows <= 240);
    uint16_t *rotated = output + 160 * rows * 8;
    int input_pitch = rotate_180 ? -240 : 240;
    int output_pitch = rotate_180 ? 160 : -160;

    for (unsigned y = 0; y < rows; y += 4)
        for (unsigned x = 0; x < 160; x += 4)
        {
            const uint16_t *source = rotate_180 ?
                    input + (159 - x) * 240 + first + y :
                    input + x * 240 + 236 - first - y;
            uint16_t *dest = rotated +
                    (rotate_180 ? y : y + 3) * 160 + x;
#if CONFIG_GPSP_LCD_DSI_SIMD
            if (!(((uintptr_t)dest | (uintptr_t)source) % 8))
            {
                gpsp_display_transpose4_p4(dest, source,
                        input_pitch * 2, output_pitch * 2);
                continue;
            }
#endif
            for (int row = 0; row < 4; row++)
                for (int col = 0; col < 4; col++)
                    dest[col * output_pitch + row] =
                            source[row * input_pitch + col];
        }
    /* Forward expansion reaches these pixels only after consuming them. */
    gpsp_display_expand(output, rotated, 160, rows, 480, 3,
            grid_x, grid_y, lut);
}
