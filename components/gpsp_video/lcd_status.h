/* Shared LCD statistics layout and formatted values.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_LCD_STATUS_H
#define GPSP_LCD_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#define LCD_TEXT_MAX 8
#define LCD_COLOR_LABEL 0x8410

enum lcd_field
{
    LCD_FIELD_FPS,
    LCD_FIELD_SPEED,
    LCD_FIELD_CPU0,
    LCD_FIELD_CPU1,
    LCD_FIELD_TEMP,
    LCD_FIELD_RAM,
    LCD_FIELD_MIN,
    LCD_FIELD_PSRAM,
    LCD_FIELD_PSRAM_SIZE,
    LCD_FIELD_COUNT,
};

struct lcd_status_position
{
    unsigned column;
    unsigned chars;
    bool right;
    bool bottom;
};

static const struct lcd_status_position lcd_status_positions[LCD_FIELD_COUNT] = {
    [LCD_FIELD_FPS] = {14, 4, true, false},
    [LCD_FIELD_SPEED] = {4, 4, true, false},
    [LCD_FIELD_CPU0] = {4, 4, false, false},
    [LCD_FIELD_CPU1] = {9, 4, false, false},
    [LCD_FIELD_TEMP] = {14, 5, false, false},
    [LCD_FIELD_RAM] = {9, 4, false, true},
    [LCD_FIELD_MIN] = {18, 4, false, true},
    [LCD_FIELD_PSRAM] = {8, 4, true, true},
    [LCD_FIELD_PSRAM_SIZE] = {4, 4, true, true},
};

struct lcd_status_label
{
    struct lcd_status_position position;
    const char *text;
};

static const struct lcd_status_label lcd_status_labels[] = {
    {{18, 3, true, false}, "FPS"},
    {{0, 3, false, false}, "CPU"},
    {{8, 3, true, false}, "SPD"},
    {{0, 8, false, true}, "RAM FREE"},
    {{14, 3, false, true}, "MIN"},
    {{11, 2, true, true}, "PS"},
};

struct lcd_status_value
{
    char text[LCD_TEXT_MAX + 1];
    uint16_t color;
};

#endif
