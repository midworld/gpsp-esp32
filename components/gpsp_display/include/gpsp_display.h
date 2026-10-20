/* RGB565 display effects.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_DISPLAY_H
#define GPSP_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

enum gpsp_display_filter
{
    GPSP_DISPLAY_NEAREST,
    GPSP_DISPLAY_BILINEAR,
    GPSP_DISPLAY_LCD_GRID,
    GPSP_DISPLAY_FILTER_COUNT,
};

enum gpsp_display_color
{
    GPSP_DISPLAY_COLOR_NONE,
    GPSP_DISPLAY_COLOR_GBA,
    GPSP_DISPLAY_COLOR_COUNT,
};

#define GPSP_DISPLAY_LUT_SIZE 65536

/* Build the approximate GBA LCD profile for every RGB565 input. */
void gpsp_display_gba_lut(uint16_t *lut);

/* Expand a rotated source directly into the scanout buffer. Pitch is in
   pixels; width is at most 240, scale is 2 or 3. Grid positions are 0 or
   2 within a 3x block, or both -1 for nearest. A NULL LUT keeps colors. */
void gpsp_display_expand(uint16_t *output, const uint16_t *input,
        unsigned width, unsigned height, unsigned pitch, unsigned scale,
        int grid_x, int grid_y, const uint16_t *lut);

/* Rotate and expand columns of a 240x160 source into a 480-wide tile.
   First and rows are multiples of four; output holds rows * 3 lines.
   Temporary source pixels share the unused end of the output tile. */
void gpsp_display_rotate_expand(uint16_t *output, const uint16_t *input,
        unsigned first, unsigned rows, bool rotate_180,
        int grid_x, int grid_y, const uint16_t *lut);

#endif
