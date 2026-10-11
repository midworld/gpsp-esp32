/* Drawing on the full-screen UI picture.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_ui.h"

#include <stdbool.h>
#include <string.h>

#include "gpsp_board.h"
#include "gpsp_screenshot.h"

#define GPSP_UI_COMMANDS 8

/* Input queues cover receive bursts; this queue defers child-screen commands.
   Each queue reports failure when its own capacity is reached. */
static struct gpsp_board_request s_commands[GPSP_UI_COMMANDS];
static unsigned s_command_head;
static unsigned s_command_count;
static bool s_in_game = true;

void gpsp_ui_fill(uint16_t *pixels, int x, int y, int w, int h,
        uint16_t color)
{
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            pixels[row * GPSP_UI_WIDTH + col] = color;
}

void gpsp_ui_tint(uint16_t *pixels, int x, int y, int w, int h,
        uint16_t color)
{
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
        {
            uint16_t *pixel = &pixels[row * GPSP_UI_WIDTH + col];

            *pixel = gpsp_ui_blend(*pixel, color);
        }
}

void gpsp_ui_frame(uint16_t *pixels, int x, int y, int w, int h,
        uint16_t color)
{
    gpsp_ui_fill(pixels, x, y, w, 1, color);
    gpsp_ui_fill(pixels, x, y + h - 1, w, 1, color);
    gpsp_ui_fill(pixels, x, y, 1, h, color);
    gpsp_ui_fill(pixels, x + w - 1, y, 1, h, color);
}

void gpsp_ui_panel(uint16_t *pixels)
{
    gpsp_ui_tint(pixels, GPSP_UI_PANEL_X, GPSP_UI_PANEL_Y, GPSP_UI_PANEL_W,
            GPSP_UI_PANEL_H, GPSP_UI_COLOR_PANEL);
    gpsp_ui_frame(pixels, GPSP_UI_PANEL_X, GPSP_UI_PANEL_Y, GPSP_UI_PANEL_W,
            GPSP_UI_PANEL_H, GPSP_UI_COLOR_BORDER);
}

void gpsp_ui_backdrop(uint16_t *pixels, const uint16_t *frame)
{
    const int x0 = (GPSP_UI_WIDTH - GPSP_UI_FRAME_WIDTH) / 2;
    const int y0 = (GPSP_UI_HEIGHT - GPSP_UI_FRAME_HEIGHT) / 2;

    memset(pixels, 0, GPSP_UI_WIDTH * GPSP_UI_HEIGHT * 2);
    if (frame)
        for (int y = 0; y < GPSP_UI_FRAME_HEIGHT; y++)
            for (int x = 0; x < GPSP_UI_FRAME_WIDTH; x++)
                pixels[(y0 + y) * GPSP_UI_WIDTH + x0 + x] =
                        (frame[y * GPSP_UI_FRAME_WIDTH + x] >> 1) & 0x7bef;
}

void gpsp_ui_half_frame(uint16_t *pixels, int x, int y,
        const uint16_t *frame)
{
    for (int row = 0; row < GPSP_UI_FRAME_HEIGHT / 2; row++)
        for (int col = 0; col < GPSP_UI_FRAME_WIDTH / 2; col++)
        {
            const uint16_t *p = frame + 2 * row * GPSP_UI_FRAME_WIDTH + 2 * col;

            pixels[(y + row) * GPSP_UI_WIDTH + x + col] = gpsp_ui_blend(
                    gpsp_ui_blend(p[0], p[1]),
                    gpsp_ui_blend(p[GPSP_UI_FRAME_WIDTH],
                            p[GPSP_UI_FRAME_WIDTH + 1]));
        }
}

static void gpsp_ui_char(uint16_t *pixels, int x, int y, char c,
        uint16_t color)
{
    const uint8_t *glyph = gpsp_font_glyph(c);

    for (int row = 0; row < GPSP_UI_FONT_HEIGHT; row++)
        for (int col = 0; col < GPSP_UI_FONT_WIDTH; col++)
            if (glyph[row] >> col & 1)
                pixels[(y + row) * GPSP_UI_WIDTH + x + col] = color;
}

/* The next character as ASCII, or '?'; UTF-8 sequences count as one. */
static char gpsp_ui_next(const char **text)
{
    unsigned char c = (unsigned char)*(*text)++;

    if (c >= 0x80)
    {
        while (((unsigned char)**text & 0xc0) == 0x80)
            (*text)++;
        return '?';
    }
    return c < ' ' || c > '~' ? '?' : (char)c;
}

void gpsp_ui_ascii(const char *text, char *out, size_t size)
{
    size_t length = 0;

    while (*text && length + 1 < size)
        out[length++] = gpsp_ui_next(&text);
    out[length] = '\0';
}

void gpsp_ui_text(uint16_t *pixels, int x, int y, const char *text,
        int max_chars, uint16_t color)
{
    const char *end = text;
    int length = 0;
    bool cut;

    while (*end && length <= max_chars)
    {
        gpsp_ui_next(&end);
        length++;
    }
    cut = length > max_chars;

    for (int i = 0; *text && i < max_chars; i++)
    {
        char c = gpsp_ui_next(&text);

        if (cut && i >= max_chars - 3)
            c = '.';
        gpsp_ui_char(pixels, x + i * GPSP_UI_FONT_WIDTH, y, c, color);
    }
}

int gpsp_ui_text_width(const char *text)
{
    int count = 0;

    while (*text)
    {
        gpsp_ui_next(&text);
        count++;
    }
    return count * GPSP_UI_FONT_WIDTH;
}

void gpsp_ui_text_clip(uint16_t *pixels, int x, int y, const char *text,
        int offset, int width, uint16_t color)
{
    for (int left = x - offset; *text && left < x + width;
            left += GPSP_UI_FONT_WIDTH)
    {
        const uint8_t *glyph = gpsp_font_glyph(gpsp_ui_next(&text));

        for (int col = 0; col < GPSP_UI_FONT_WIDTH; col++)
        {
            int px = left + col;

            if (px < x || px >= x + width)
                continue;
            for (int row = 0; row < GPSP_UI_FONT_HEIGHT; row++)
                if (glyph[row] >> col & 1)
                    pixels[(y + row) * GPSP_UI_WIDTH + px] = color;
        }
    }
}

/* Waits at either end, then one pixel per step. */
#define GPSP_UI_MARQUEE_WAIT_US 1000000
#define GPSP_UI_MARQUEE_STEP_US 30000

void gpsp_ui_marquee_reset(struct gpsp_ui_marquee *marquee, int64_t now_us)
{
    marquee->offset = 0;
    marquee->next_us = now_us + GPSP_UI_MARQUEE_WAIT_US;
}

bool gpsp_ui_marquee_step(struct gpsp_ui_marquee *marquee, int text_width,
        int box_width, int64_t now_us)
{
    int end = text_width - box_width;

    if (end <= 0 || now_us < marquee->next_us)
        return false;
    if (marquee->offset >= end)
    {
        gpsp_ui_marquee_reset(marquee, now_us);
        return true;
    }
    marquee->offset++;
    marquee->next_us = now_us + (marquee->offset >= end ?
            GPSP_UI_MARQUEE_WAIT_US : GPSP_UI_MARQUEE_STEP_US);
    return true;
}

static void gpsp_ui_discard_commands(void)
{
    unsigned kept = 0;

    for (unsigned i = 0; i < s_command_count; i++)
    {
        struct gpsp_board_request request =
                s_commands[(s_command_head + i) % GPSP_UI_COMMANDS];

        if (gpsp_board_command_valid(request))
            s_commands[(s_command_head + kept++) % GPSP_UI_COMMANDS] = request;
    }
    s_command_count = kept;
}

static void gpsp_ui_hold_command(struct gpsp_board_request request)
{
    if (s_command_count == GPSP_UI_COMMANDS)
    {
        gpsp_board_command_report(request, false);
        return;
    }
    s_commands[(s_command_head + s_command_count) % GPSP_UI_COMMANDS] =
            request;
    s_command_count++;
}

static void gpsp_ui_drop_commands(void)
{
    while (s_command_count)
    {
        gpsp_board_command_report(s_commands[s_command_head], false);
        s_command_head = (s_command_head + 1) % GPSP_UI_COMMANDS;
        s_command_count--;
    }
}

void gpsp_ui_set_game(bool in_game)
{
    s_in_game = in_game;
    if (!in_game)
        gpsp_ui_drop_commands();
}

bool gpsp_ui_poll_command(const uint16_t *pixels)
{
    gpsp_ui_discard_commands();
    struct gpsp_board_request request = gpsp_board_command_read();

    if (request.command == GPSP_BOARD_COMMAND_SCREENSHOT)
        gpsp_screenshot_print(pixels, GPSP_UI_WIDTH, GPSP_UI_HEIGHT,
                GPSP_UI_WIDTH * 2);
    else if (request.command == GPSP_BOARD_COMMAND_QUIT)
    {
        gpsp_ui_drop_commands();
        return true;
    }
    else if (request.command != GPSP_BOARD_COMMAND_NONE &&
            gpsp_board_command_valid(request))
    {
        if (!s_in_game)
            gpsp_board_command_report(request, false);
        else
            gpsp_ui_hold_command(request);
    }
    return false;
}

struct gpsp_board_request gpsp_ui_command(void)
{
    /* Discard disconnected requests without displacing serial commands. */
    gpsp_ui_discard_commands();
    struct gpsp_board_request request = gpsp_board_command_read();
    struct gpsp_board_request held;

    if (request.command == GPSP_BOARD_COMMAND_QUIT)
    {
        gpsp_ui_drop_commands();
        return request;
    }
    if (!s_command_count || request.command == GPSP_BOARD_COMMAND_SCREENSHOT)
        return request;
    held = s_commands[s_command_head];
    s_command_head = (s_command_head + 1) % GPSP_UI_COMMANDS;
    s_command_count--;
    if (request.command != GPSP_BOARD_COMMAND_NONE)
        gpsp_ui_hold_command(request);
    return held;
}
