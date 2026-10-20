/* Compare tile rotation and expansion against source-coordinate pixels.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "gpsp_display.h"

static uint16_t dim(uint16_t value)
{
    unsigned r = ((value >> 11) * 3 + 2) / 4;
    unsigned g = (((value >> 5) & 63) * 3 + 2) / 4;
    unsigned b = ((value & 31) * 3 + 2) / 4;
    return (r << 11) | (g << 5) | b;
}

static void check(unsigned rows, bool reverse, bool grid, bool color,
        unsigned unaligned)
{
    unsigned count = 480 * rows * 3;
    uint16_t *storage = malloc((240 * 160 + 8) * sizeof(*storage));
    uint16_t *input = storage + unaligned;
    uint16_t *allocation = malloc((count + 32) * sizeof(*allocation));
    uint16_t *output = allocation + 16;
    uint16_t *lut = malloc(GPSP_DISPLAY_LUT_SIZE * sizeof(*lut));
    assert(storage && allocation && lut);
    for (unsigned i = 0; i < 240 * 160; i++)
        input[i] = (uint16_t)(i * 577 + i / 240 * 19);
    for (unsigned i = 0; i < GPSP_DISPLAY_LUT_SIZE; i++)
        lut[i] = (uint16_t)((i * 5) ^ 0x5a5a);
    int gx = grid ? (reverse ? 0 : 2) : -1;
    int gy = grid ? (reverse ? 2 : 0) : -1;
    for (unsigned first = 0; first < 240; first += rows)
    {
        for (unsigned i = 0; i < count + 32; i++)
            allocation[i] = 0xdead;
        gpsp_display_rotate_expand(output, input, first, rows,
                reverse, gx, gy, color ? lut : NULL);
        for (unsigned y = 0; y < rows * 3; y++)
            for (unsigned x = 0; x < 480; x++)
            {
                unsigned sx = reverse ? first + y / 3 : 239 - first - y / 3;
                unsigned sy = reverse ? 159 - x / 3 : x / 3;
                uint16_t expected = input[sy * 240 + sx];
                if (color)
                    expected = lut[expected];
                if (grid && (int)(x % 3) == gx)
                    expected = dim(expected);
                if (grid && (int)(y % 3) == gy)
                    expected = dim(expected);
                assert(output[y * 480 + x] == expected);
            }
        for (unsigned i = 0; i < 16; i++)
            assert(allocation[i] == 0xdead && output[count + i] == 0xdead);
    }
    free(lut);
    free(allocation);
    free(storage);
}

int main(void)
{
    const unsigned rows[] = {8, 12, 16, 240};
    for (unsigned r = 0; r < sizeof(rows) / sizeof(rows[0]); r++)
        for (unsigned reverse = 0; reverse < 2; reverse++)
            for (unsigned grid = 0; grid < 2; grid++)
                for (unsigned color = 0; color < 2; color++)
                    for (unsigned unaligned = 0; unaligned < 2; unaligned++)
                        check(rows[r], reverse, grid, color, unaligned);
    puts("DISPLAY_TEST PASS: rotations, tile sizes, effects, overlap and bounds");
    return 0;
}
