/* RGB565 bars, fine detail, and motion for SPI signal checks.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "lcd_test_pattern.h"

#include <stddef.h>
#include <string.h>

struct lcd_test_glyph
{
    char character;
    uint8_t rows[7];
};

static const struct lcd_test_glyph s_glyphs[] = {
    {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},
    {'M', {17, 27, 21, 21, 17, 17, 17}},
    {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'Z', {31, 1, 2, 4, 8, 16, 31}},
};

static void lcd_test_rect(uint16_t *pixels,
        unsigned x, unsigned y, unsigned width, unsigned height,
        uint16_t color)
{
    unsigned row;
    unsigned column;

    for (row = 0; row < height; row++)
        for (column = 0; column < width; column++)
            pixels[(y + row) * LCD_TEST_WIDTH + x + column] = color;
}

static void lcd_test_text(uint16_t *pixels,
        unsigned x, unsigned y, const char *text)
{
    while (*text)
    {
        size_t glyph;

        for (glyph = 0; glyph < sizeof(s_glyphs) / sizeof(s_glyphs[0]); glyph++)
        {
            unsigned row;
            unsigned column;

            if (s_glyphs[glyph].character != *text)
                continue;

            for (row = 0; row < 7; row++)
                for (column = 0; column < 5; column++)
                    if (s_glyphs[glyph].rows[row] & (1u << (4 - column)))
                        lcd_test_rect(pixels, x + column * 3, y + row * 3,
                                3, 3, 0xffff);
            break;
        }

        x += 18;
        text++;
    }
}

void lcd_test_pattern_draw(
        uint16_t *pixels, unsigned clock_mhz, uint32_t frame_number)
{
    static const uint16_t colors[] = {
        0xf800, 0x07e0, 0x001f, 0xffff,
        0x0000, 0xffe0, 0x07ff, 0xf81f,
    };
    unsigned x;
    unsigned y;
    unsigned bit;

    for (x = 0; x < LCD_TEST_WIDTH; x++)
        pixels[x] = colors[x / 30];
    for (y = 1; y < LCD_TEST_HEIGHT; y++)
        memcpy(pixels + y * LCD_TEST_WIDTH,
                pixels, LCD_TEST_WIDTH * sizeof(uint16_t));

    lcd_test_rect(pixels, 59, 5, 122, 29, 0x0000);
    lcd_test_text(pixels, 67, 9,
            clock_mhz == 10 ? "10 MHZ" :
            clock_mhz == 20 ? "20 MHZ" : "40 MHZ");

    x = 1 + (frame_number * 3u) % (LCD_TEST_WIDTH - 26);
    y = 55 + (frame_number / 3u) % 45;
    lcd_test_rect(pixels, x, y, 24, 24, 0xffff);
    lcd_test_rect(pixels, x + 2, y + 2, 20, 20, 0x0000);

    for (y = 130; y < 149; y++)
        for (x = 1; x < LCD_TEST_WIDTH - 1; x++)
            pixels[y * LCD_TEST_WIDTH + x] =
                    ((x >> 1) ^ (y >> 1) ^ (frame_number >> 3)) & 1u ?
                    0xffff : 0x0000;

    for (bit = 0; bit < 16; bit++)
        lcd_test_rect(pixels, 24 + bit * 12, 153, 10, 5,
                frame_number & (1u << bit) ? 0xffff : 0x0000);

    lcd_test_rect(pixels, 0, 0, LCD_TEST_WIDTH, 1, 0xffff);
    lcd_test_rect(pixels, 0, LCD_TEST_HEIGHT - 1,
            LCD_TEST_WIDTH, 1, 0xffff);
    lcd_test_rect(pixels, 0, 0, 1, LCD_TEST_HEIGHT, 0xffff);
    lcd_test_rect(pixels, LCD_TEST_WIDTH - 1, 0,
            1, LCD_TEST_HEIGHT, 0xffff);
}
