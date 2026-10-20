/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_VIRTUAL_PAD_H
#define GPSP_VIRTUAL_PAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPSP_TOUCH_CONTACTS 10
#define GPSP_VIRTUAL_PAD_VISIBLE (1u << 31)
#define GPSP_VIRTUAL_PAD_HOME (1u << 16)

enum gpsp_pad_region
{
    GPSP_PAD_SELECT, GPSP_PAD_START, GPSP_PAD_HOME,
    GPSP_PAD_L, GPSP_PAD_R, GPSP_PAD_DPAD, GPSP_PAD_AB,
    GPSP_PAD_REGIONS,
};

struct gpsp_pad_rect { unsigned x, y, width, height; };
extern const struct gpsp_pad_rect gpsp_pad_rects[GPSP_PAD_REGIONS];

struct gpsp_touch_contact { uint16_t x, y; bool down; };
struct gpsp_virtual_pad
{
    bool enabled, blocked, tapping, first_tap;
    uint32_t buttons, tap_start, tap_end;
    unsigned tap_x, tap_y, first_x, first_y;
    int groups[GPSP_TOUCH_CONTACTS];
    uint32_t held[GPSP_TOUCH_CONTACTS];
};

void gpsp_virtual_pad_enable(struct gpsp_virtual_pad *pad, bool enabled);
void gpsp_virtual_pad_cancel(struct gpsp_virtual_pad *pad);
void gpsp_virtual_pad_update(struct gpsp_virtual_pad *pad,
        const struct gpsp_touch_contact *contacts, uint32_t now_ms);
/* ST7123 reports have a four-byte header and seven bytes per contact. */
bool gpsp_st7123_decode(const uint8_t *report, size_t size, unsigned count,
        unsigned width, unsigned height, bool rotate_180,
        struct gpsp_touch_contact *contacts);

#endif
