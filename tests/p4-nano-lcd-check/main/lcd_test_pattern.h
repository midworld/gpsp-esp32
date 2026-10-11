/* LCD test pattern.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_LCD_TEST_PATTERN_H
#define GPSP_LCD_TEST_PATTERN_H

#include <stdint.h>

#define LCD_TEST_WIDTH 240
#define LCD_TEST_HEIGHT 160
#define LCD_TEST_PIXELS (LCD_TEST_WIDTH * LCD_TEST_HEIGHT)
#define LCD_TEST_BYTES (LCD_TEST_PIXELS * sizeof(uint16_t))

void lcd_test_pattern_draw(
        uint16_t *pixels, unsigned clock_mhz, uint32_t frame_number);

#endif /* GPSP_LCD_TEST_PATTERN_H */
