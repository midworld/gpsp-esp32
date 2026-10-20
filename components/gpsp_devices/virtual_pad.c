/* Touch contacts to fixed GBA buttons, independent of physical pad mappings.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_virtual_pad.h"
#include "libretro.h"
#include <string.h>

#define BUTTON(name) (1u << RETRO_DEVICE_ID_JOYPAD_##name)

const struct gpsp_pad_rect gpsp_pad_rects[GPSP_PAD_REGIONS] = {
    [GPSP_PAD_SELECT] = {0, 0, 144, 52},
    [GPSP_PAD_START] = {656, 0, 144, 52},
    [GPSP_PAD_HOME] = {358, 0, 84, 52},
    [GPSP_PAD_L] = {0, 150, 144, 64},
    [GPSP_PAD_R] = {656, 150, 144, 64},
    [GPSP_PAD_DPAD] = {0, 238, 216, 216},
    [GPSP_PAD_AB] = {568, 248, 232, 206},
};

static unsigned distance(int x, int y) { return x * x + y * y; }

void gpsp_virtual_pad_cancel(struct gpsp_virtual_pad *pad)
{
    pad->buttons = 0;
    pad->blocked = true;
    pad->tapping = pad->first_tap = false;
    memset(pad->groups, -1, sizeof(pad->groups));
    memset(pad->held, 0, sizeof(pad->held));
}

void gpsp_virtual_pad_enable(struct gpsp_virtual_pad *pad, bool enabled)
{
    pad->enabled = enabled;
    gpsp_virtual_pad_cancel(pad);
}

static bool inside(const struct gpsp_pad_rect *r, unsigned x, unsigned y)
{
    return x >= r->x && x < r->x + r->width &&
            y >= r->y && y < r->y + r->height;
}

static uint32_t pad_buttons(unsigned group, int x, int y, uint32_t previous)
{
    static const uint32_t masks[] = {
        BUTTON(SELECT), BUTTON(START), GPSP_VIRTUAL_PAD_HOME,
        BUTTON(L), BUTTON(R),
    };
    if (group == GPSP_PAD_DPAD)
    {
        int dx = x - 108, dy = y - 346;
        unsigned ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        unsigned neutral = previous ? 18 : 26;
        uint32_t mask = 0;

        if (distance(dx, dy) < neutral * neutral || ax > 130 || ay > 130)
            return 0;
        unsigned hx = previous & (BUTTON(LEFT) | BUTTON(RIGHT)) ? 35 : 48;
        unsigned hy = previous & (BUTTON(UP) | BUTTON(DOWN)) ? 35 : 48;
        if (ax * 100 > ay * hx)
            mask |= dx < 0 ? BUTTON(LEFT) : BUTTON(RIGHT);
        if (ay * 100 > ax * hy)
            mask |= dy < 0 ? BUTTON(UP) : BUTTON(DOWN);
        return mask;
    }
    if (group == GPSP_PAD_AB)
    {
        uint32_t mask = 0;
        if (distance(x - 742, y - 306) <= 76 * 76)
            mask |= BUTTON(A);
        if (distance(x - 626, y - 396) <= 76 * 76)
            mask |= BUTTON(B);
        return mask;
    }
    return group < GPSP_PAD_DPAD && inside(&gpsp_pad_rects[group], x, y) ?
            masks[group] : 0;
}

static void pad_double_tap(struct gpsp_virtual_pad *pad,
        const struct gpsp_touch_contact *contacts, unsigned count, uint32_t now)
{
    const struct gpsp_touch_contact *point = NULL;
    for (unsigned i = 0; i < GPSP_TOUCH_CONTACTS; i++)
        if (contacts[i].down)
            point = &contacts[i];
    if (count > 1)
    {
        pad->blocked = true;
        pad->tapping = pad->first_tap = false;
    }
    else if (point)
    {
        if (!pad->tapping)
        {
            pad->tapping = true;
            pad->tap_start = now;
            pad->tap_x = point->x;
            pad->tap_y = point->y;
        }
        else if (distance(point->x - (int)pad->tap_x,
                point->y - (int)pad->tap_y) > 24 * 24)
        {
            pad->blocked = true;
            pad->tapping = pad->first_tap = false;
        }
    }
    else if (pad->tapping)
    {
        pad->tapping = false;
        if (now - pad->tap_start > 220)
            pad->first_tap = false;
        else if (pad->first_tap && now - pad->tap_end <= 350 &&
                distance(pad->tap_x - (int)pad->first_x,
                        pad->tap_y - (int)pad->first_y) <= 60 * 60)
            gpsp_virtual_pad_enable(pad, true);
        else
        {
            pad->first_tap = true;
            pad->tap_end = now;
            pad->first_x = pad->tap_x;
            pad->first_y = pad->tap_y;
        }
    }
}

void gpsp_virtual_pad_update(struct gpsp_virtual_pad *pad,
        const struct gpsp_touch_contact *contacts, uint32_t now_ms)
{
    unsigned count = 0;

    for (unsigned i = 0; i < GPSP_TOUCH_CONTACTS; i++)
        count += contacts[i].down;
    pad->buttons = 0;
    if (pad->blocked)
    {
        if (!count)
            pad->blocked = false;
        return;
    }
    if (!pad->enabled)
    {
        pad_double_tap(pad, contacts, count, now_ms);
        return;
    }
    for (unsigned i = 0; i < GPSP_TOUCH_CONTACTS; i++)
    {
        if (!contacts[i].down)
        {
            pad->groups[i] = -1;
            pad->held[i] = 0;
            continue;
        }
        unsigned x = contacts[i].x, y = contacts[i].y;
        if (pad->groups[i] < 0)
        {
            pad->groups[i] = GPSP_PAD_REGIONS;
            for (unsigned group = 0; group < GPSP_PAD_REGIONS; group++)
                if (inside(&gpsp_pad_rects[group], x, y))
                {
                    pad->groups[i] = group;
                    break;
                }
        }
        pad->held[i] = pad_buttons(pad->groups[i], x, y, pad->held[i]);
        pad->buttons |= pad->held[i];
    }
}

bool gpsp_st7123_decode(const uint8_t *report, size_t size, unsigned count,
        unsigned width, unsigned height, bool rotate_180,
        struct gpsp_touch_contact *contacts)
{
    memset(contacts, 0, sizeof(*contacts) * GPSP_TOUCH_CONTACTS);
    if (!width || !height || count > GPSP_TOUCH_CONTACTS ||
            size < 4 + count * 7 || (report[0] & 0x80))
        return false;
    if (!(report[0] & 8))
        return true;
    for (unsigned i = 0; i < count; i++)
    {
        const uint8_t *p = report + 4 + i * 7;
        unsigned x = ((p[0] & 63) << 8) | p[1];
        unsigned y = ((p[2] & 63) << 8) | p[3];

        if (!(p[0] & 0x80))
            continue;
        if (x >= width || y >= height)
            return false;
        x = x * 480 / width;
        y = y * 800 / height;
        contacts[i] = (struct gpsp_touch_contact){
            .x = rotate_180 ? y : 799 - y,
            .y = rotate_180 ? 479 - x : x,
            .down = true,
        };
    }
    return true;
}
