/* Drawing on the full-screen UI picture.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_UI_H
#define GPSP_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpsp_font.h"

#include "gpsp_board.h"

#define GPSP_UI_WIDTH 320
#define GPSP_UI_HEIGHT 240
#define GPSP_UI_FONT_WIDTH GPSP_FONT_WIDTH
#define GPSP_UI_FONT_HEIGHT GPSP_FONT_HEIGHT
/* Game frames, as the backdrop and as state pictures. */
#define GPSP_UI_FRAME_WIDTH 240
#define GPSP_UI_FRAME_HEIGHT 160

/* The panel that the menu and the game list draw on. */
#define GPSP_UI_PANEL_X 20
#define GPSP_UI_PANEL_Y 16
#define GPSP_UI_PANEL_W 280
#define GPSP_UI_PANEL_H 208
#define GPSP_UI_LINE 16

#define GPSP_UI_COLOR_PANEL 0x0849
#define GPSP_UI_COLOR_BORDER 0x8410
#define GPSP_UI_COLOR_TITLE 0xffff
#define GPSP_UI_COLOR_TEXT 0xd69a
#define GPSP_UI_COLOR_DIM 0x8410
#define GPSP_UI_COLOR_CURSOR 0x3b3f
#define GPSP_UI_COLOR_EMPTY 0x2104

/* Half way between two RGB565 colors. */
static inline uint16_t gpsp_ui_blend(uint16_t a, uint16_t b)
{
    return ((a & 0xf7de) >> 1) + ((b & 0xf7de) >> 1);
}

void gpsp_ui_fill(uint16_t *pixels, int x, int y, int w, int h,
        uint16_t color);

/* Blends a rectangle half way to the color. */
void gpsp_ui_tint(uint16_t *pixels, int x, int y, int w, int h,
        uint16_t color);

/* A one-pixel frame inside the rectangle. */
void gpsp_ui_frame(uint16_t *pixels, int x, int y, int w, int h,
        uint16_t color);

/* The see-through panel with its border. */
void gpsp_ui_panel(uint16_t *pixels);

/* The game frame dimmed by half in the middle, or black without one. */
void gpsp_ui_backdrop(uint16_t *pixels, const uint16_t *frame);

/* A game frame at half size, each pixel the mean of four. */
void gpsp_ui_half_frame(uint16_t *pixels, int x, int y,
        const uint16_t *frame);

/* UTF-8 text as the font shows it: other characters become '?'. */
void gpsp_ui_ascii(const char *text, char *out, size_t size);

/**
 * Draw UTF-8 text, at most max_chars characters. Characters outside
 * printable ASCII show as '?'. Longer text ends in "...".
 */
void gpsp_ui_text(uint16_t *pixels, int x, int y, const char *text,
        int max_chars, uint16_t color);

/* The width of UTF-8 text in pixels, one cell per character. */
int gpsp_ui_text_width(const char *text);

/**
 * Draw UTF-8 text shifted left by offset pixels, cut to width pixels
 * without "...".
 */
void gpsp_ui_text_clip(uint16_t *pixels, int x, int y, const char *text,
        int offset, int width, uint16_t color);

/* How a screen opened from the menu ended. */
enum gpsp_ui_exit
{
    /* It got its answer. */
    GPSP_UI_DONE,
    /* Back to the page that opened it. */
    GPSP_UI_BACK,
    /* Home: leave the menu for the game. */
    GPSP_UI_RESUME,
    /* The host asked to close the game. */
    GPSP_UI_QUIT,
};

/**
 * Read a host command inside a menu screen. Screenshots are taken at once;
 * state commands wait in order for gpsp_ui_command(). Without a game they
 * fail at once. A full queue reports failure; quit cancels queued commands.
 * Disconnected TCP requests are discarded before execution.
 *
 * @return true when the host asks to close the game.
 */
bool gpsp_ui_poll_command(const uint16_t *pixels);

/* Set the game context; clearing it fails queued state commands. */
void gpsp_ui_set_game(bool in_game);

/** The next host command, with quit and screenshots handled first. */
struct gpsp_board_request gpsp_ui_command(void);

/* Text too long for its box scrolls: it waits, slides to its end, waits,
   then starts over. */
struct gpsp_ui_marquee
{
    int offset;
    int64_t next_us;
};

void gpsp_ui_marquee_reset(struct gpsp_ui_marquee *marquee, int64_t now_us);

/** Move the text on; true when the offset changed and it needs drawing. */
bool gpsp_ui_marquee_step(struct gpsp_ui_marquee *marquee, int text_width,
        int box_width, int64_t now_us);

#endif /* GPSP_UI_H */
