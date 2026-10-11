/* 7x13 bitmap font for the menu and the LCD status.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_FONT_H
#define GPSP_FONT_H

#include <stdint.h>

#define GPSP_FONT_WIDTH 7
#define GPSP_FONT_HEIGHT 13

/* Printable ASCII from ' ' to '~'; one byte per row, top first, with
   bit 0 the leftmost column. */
extern const uint8_t gpsp_font[95][GPSP_FONT_HEIGHT];

/** The glyph of a character, or '?' for one outside printable ASCII. */
static inline const uint8_t *gpsp_font_glyph(char c)
{
    return gpsp_font[(c < ' ' || c > '~' ? '?' : c) - ' '];
}

#endif /* GPSP_FONT_H */
