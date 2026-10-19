/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "espemu_protocol.h"
#include "gpsp_board.h"
#include "gpsp_rv32_platform.h"

static uint32_t session;
static bool submit_ok = true;
static unsigned ui_frames;
const struct gpsp_rv32_platform_ops gpsp_esp_rv32_platform = {0};

void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return calloc(1, size);
}

uint32_t gpsp_espemu_transport_session(void) { return session; }
uint16_t gpsp_espemu_transport_read_input(void) { return 0; }

bool gpsp_espemu_transport_submit_frame(
        const uint16_t *pixels, unsigned width, unsigned height, size_t pitch)
{
    assert(pixels && pitch == width * 2);
    if (width == GPSP_ESPEMU_UI_WIDTH && height == GPSP_ESPEMU_UI_HEIGHT &&
            session && submit_ok)
        ui_frames++;
    return session && submit_ok;
}

int main(void)
{
    static uint16_t game[GPSP_ESPEMU_SCREEN_WIDTH * GPSP_ESPEMU_SCREEN_HEIGHT];
    uint16_t *pixels;
    const uint16_t *frame;
    enum gpsp_display_filter filter;
    enum gpsp_display_color color;

    assert(!gpsp_board_display_options(&filter, &color));
    gpsp_board_set_display_options(GPSP_DISPLAY_LCD_GRID, GPSP_DISPLAY_COLOR_GBA);
    assert(!gpsp_board_display_options(&filter, &color));
    assert(filter == GPSP_DISPLAY_NEAREST && color == GPSP_DISPLAY_COLOR_NONE);

    /* The menu shows the last game frame behind it. */
    gpsp_board_video_present(game, GPSP_ESPEMU_SCREEN_WIDTH,
            GPSP_ESPEMU_SCREEN_HEIGHT, GPSP_ESPEMU_SCREEN_PITCH);
    assert(gpsp_board_ui_begin(&pixels, &frame));
    assert(pixels && frame == game);

    /* Drawn before a viewer connects: the first connection gets it. */
    gpsp_board_ui_draw(0, GPSP_ESPEMU_UI_HEIGHT);
    gpsp_board_input_read();
    assert(ui_frames == 0);
    session = 1;
    gpsp_board_input_read();
    assert(ui_frames == 1);
    gpsp_board_input_read();
    assert(ui_frames == 1);

    /* A reconnect gets the unchanged picture again. */
    session = 0;
    gpsp_board_input_read();
    session = 3;
    gpsp_board_input_read();
    assert(ui_frames == 2);

    /* A failed send is retried on the next poll. */
    submit_ok = false;
    gpsp_board_ui_draw(0, 1);
    submit_ok = true;
    gpsp_board_input_read();
    assert(ui_frames == 3);
    gpsp_board_input_read();
    assert(ui_frames == 3);

    /* No menu pictures after the menu closes. */
    gpsp_board_ui_end();
    session = 5;
    gpsp_board_input_read();
    assert(ui_frames == 3);
    puts("ESP-EMU menu resend: PASS");
    return 0;
}
