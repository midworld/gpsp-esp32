/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gpsp_virtual_pad.h"
#include "libretro.h"
#define BUTTON(name) (1u << RETRO_DEVICE_ID_JOYPAD_##name)
static struct gpsp_touch_contact points[GPSP_TOUCH_CONTACTS];

static void empty(struct gpsp_virtual_pad *pad, unsigned now)
{
    memset(points, 0, sizeof(points));
    gpsp_virtual_pad_update(pad, points, now);
    assert(pad->buttons == 0);
}

static void tap(struct gpsp_virtual_pad *pad, unsigned now, unsigned x, unsigned y)
{
    points[0] = (struct gpsp_touch_contact){x, y, true};
    gpsp_virtual_pad_update(pad, points, now);
    empty(pad, now + 50);
}

static void check_taps(void)
{
    struct gpsp_virtual_pad pad = {0};
    gpsp_virtual_pad_enable(&pad, false);
    empty(&pad, 0);
    tap(&pad, 100, 400, 200);
    assert(!pad.enabled);
    tap(&pad, 300, 410, 200);
    assert(pad.enabled && pad.blocked && !pad.buttons);
    empty(&pad, 400);
    tap(&pad, 500, 400, 200);
    tap(&pad, 650, 400, 200);
    assert(pad.enabled);

    gpsp_virtual_pad_enable(&pad, false);
    empty(&pad, 1000);
    tap(&pad, 1100, 100, 200);
    tap(&pad, 1700, 100, 200);
    assert(!pad.enabled);
    tap(&pad, 1900, 500, 200);
    assert(!pad.enabled);
    empty(&pad, 2100);
    points[0] = (struct gpsp_touch_contact){500, 200, true};
    gpsp_virtual_pad_update(&pad, points, 2200);
    points[0].x = 550;
    gpsp_virtual_pad_update(&pad, points, 2210);
    empty(&pad, 2250);
    tap(&pad, 2300, 500, 200);
    assert(!pad.enabled);
    points[0].down = points[1].down = true;
    gpsp_virtual_pad_update(&pad, points, 2400);
    empty(&pad, 2450);
    tap(&pad, 2500, 500, 200);
    assert(!pad.enabled);

    gpsp_virtual_pad_enable(&pad, false);
    empty(&pad, UINT32_MAX - 200);
    tap(&pad, UINT32_MAX - 150, 300, 200);
    tap(&pad, 50, 300, 200);
    assert(pad.enabled);
}

static void check_buttons(void)
{
    struct gpsp_virtual_pad pad = {0};
    gpsp_virtual_pad_enable(&pad, true);
    empty(&pad, 0);
    points[0] = (struct gpsp_touch_contact){20, 270, true};
    points[1] = (struct gpsp_touch_contact){742, 306, true};
    points[2] = (struct gpsp_touch_contact){30, 180, true};
    gpsp_virtual_pad_update(&pad, points, 10);
    assert(pad.buttons == (BUTTON(UP) | BUTTON(LEFT) | BUTTON(A) | BUTTON(L)));
    points[0] = (struct gpsp_touch_contact){108, 346, true};
    points[1] = (struct gpsp_touch_contact){626, 396, true};
    gpsp_virtual_pad_update(&pad, points, 20);
    assert(pad.buttons == (BUTTON(B) | BUTTON(L)));
    points[1] = (struct gpsp_touch_contact){684, 351, true};
    gpsp_virtual_pad_update(&pad, points, 30);
    assert(pad.buttons == (BUTTON(A) | BUTTON(B) | BUTTON(L)));
    points[2] = (struct gpsp_touch_contact){680, 180, true};
    gpsp_virtual_pad_update(&pad, points, 40);
    assert(!(pad.buttons & BUTTON(R))); /* Captured by the L group. */
    gpsp_virtual_pad_cancel(&pad);
    gpsp_virtual_pad_update(&pad, points, 50);
    assert(!pad.buttons);
    empty(&pad, 60);
    points[0] = (struct gpsp_touch_contact){0, 26, true};
    points[1] = (struct gpsp_touch_contact){799, 77, true};
    points[2] = (struct gpsp_touch_contact){400, 50, true};
    points[3] = (struct gpsp_touch_contact){700, 180, true};
    gpsp_virtual_pad_update(&pad, points, 70);
    assert(pad.buttons == (BUTTON(SELECT) | BUTTON(START) |
            GPSP_VIRTUAL_PAD_HOME | BUTTON(R)));
    gpsp_virtual_pad_enable(&pad, false);
    assert(!pad.buttons && !pad.enabled);
    gpsp_virtual_pad_update(&pad, points, 80);
    assert(!pad.buttons && !pad.enabled);
    empty(&pad, 90);
}

static void encode(uint8_t *report, unsigned slot, unsigned x, unsigned y)
{
    uint8_t *p = report + 4 + slot * 7;
    p[0] = 0x80 | (x >> 8); p[1] = x;
    p[2] = y >> 8; p[3] = y;
}

static void check_reports(void)
{
    uint8_t report[74] = {8};
    encode(report, 0, 0, 0);
    encode(report, 4, 479, 799);
    assert(gpsp_st7123_decode(report, sizeof(report), 5, 480, 800, false, points));
    assert(points[0].down && points[0].x == 799 && points[0].y == 0);
    assert(points[4].down && points[4].x == 0 && points[4].y == 479);
    assert(!points[1].down && !points[9].down);
    assert(gpsp_st7123_decode(report, sizeof(report), 5, 480, 800, true, points));
    assert(points[0].x == 0 && points[0].y == 479);
    assert(points[4].x == 799 && points[4].y == 0);
    assert(!gpsp_st7123_decode(report, 38, 5, 480, 800, false, points));
    assert(!gpsp_st7123_decode(report, 74, 11, 480, 800, false, points));
    encode(report, 0, 480, 0);
    assert(!gpsp_st7123_decode(report, 74, 5, 480, 800, false, points));
    report[0] = 0;
    assert(gpsp_st7123_decode(report, 74, 5, 480, 800, false, points));
    for (unsigned i = 0; i < GPSP_TOUCH_CONTACTS; i++) assert(!points[i].down);
    report[0] = 0x88;
    assert(!gpsp_st7123_decode(report, 74, 5, 480, 800, false, points));
}

int main(void)
{
    check_taps();
    check_buttons();
    check_reports();
    puts("VIRTUAL_PAD PASS: gestures, multi-touch, capture, release, report bounds, rotation");
}
