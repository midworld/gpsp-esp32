/* The game list: pick a card ROM on the full-screen UI.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_loader.h"

#include "sdkconfig.h"

#if CONFIG_GPSP_MENU

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "gpsp_board.h"
#include "gpsp_settings.h"
#include "gpsp_state.h"
#include "gpsp_storage.h"
#include "gpsp_ui.h"
#include "libretro.h"

#define LOADER_POLL_MS 16
#define LOADER_REPEAT_DELAY_US 300000
#define LOADER_REPEAT_US 80000
/* More ROMs than this are left out. */
#define LOADER_MAX_ROMS 1000

#define LOADER_TEXT_X (GPSP_UI_PANEL_X + 10)
#define LOADER_TITLE_Y (GPSP_UI_PANEL_Y + 8)
#define LOADER_LIST_Y (LOADER_TITLE_Y + GPSP_UI_LINE + 8)
#define LOADER_FOOTER_Y \
    (GPSP_UI_PANEL_Y + GPSP_UI_PANEL_H - 8 - GPSP_UI_FONT_HEIGHT)
#define LOADER_ROWS ((LOADER_FOOTER_Y - 4 - LOADER_LIST_Y) / GPSP_UI_LINE)
/* Names end here; the auto state picture sits right of the list. */
#define LOADER_LIST_RIGHT 160
#define LOADER_LIST_WIDTH (LOADER_LIST_RIGHT - LOADER_TEXT_X)
#define LOADER_PREVIEW_X 170
#define LOADER_PREVIEW_W (GPSP_UI_FRAME_WIDTH / 2)
#define LOADER_PREVIEW_H (GPSP_UI_FRAME_HEIGHT / 2)
#define LOADER_NAME_CHARS (LOADER_PREVIEW_W / GPSP_UI_FONT_WIDTH)
#define LOADER_NAME_LINES 4

#define LOADER_BUTTON(id) (1u << RETRO_DEVICE_ID_JOYPAD_##id)

struct loader
{
    uint16_t *pixels;
    const uint16_t *frame;
    char **roms;
    size_t count;
    size_t cursor;
    size_t top;
    /* The cursor ROM's auto state picture; preview_rom is its index. */
    uint16_t *preview;
    size_t preview_rom;
    bool preview_ok;
    /* The cursor name scrolls when it is longer than the list. */
    struct gpsp_ui_marquee marquee;
};

static void loader_add(const char *path, void *user)
{
    struct loader *loader = user;
    size_t length = strlen(path);
    char *copy;

    if (loader->count >= LOADER_MAX_ROMS)
        return;
    copy = heap_caps_malloc(length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy)
        return;
    memcpy(copy, path, length + 1);
    loader->roms[loader->count++] = copy;
}

static const char *loader_base(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

static int loader_compare(const void *a, const void *b)
{
    return strcasecmp(loader_base(*(char *const *)a),
            loader_base(*(char *const *)b));
}

/* The file name without its extension, as the font shows it. */
static void loader_title(const char *path, char *title, size_t size)
{
    char name[256];
    const char *base = loader_base(path);
    const char *dot = strrchr(base, '.');
    size_t length = dot && dot != base ? (size_t)(dot - base) : strlen(base);

    if (length >= sizeof(name))
        length = sizeof(name) - 1;
    memcpy(name, base, length);
    name[length] = '\0';
    gpsp_ui_ascii(name, title, size);
}

static void loader_read_preview(struct loader *loader)
{
    char path[300];
    size_t length;

    if (loader->preview_rom == loader->cursor)
        return;
    loader->preview_rom = loader->cursor;
    loader->preview_ok = false;
    if (!loader->preview ||
            !gpsp_storage_state_path_for(loader->roms[loader->cursor], path,
                    sizeof(path) - sizeof(".auto.png")))
        return;
    length = strlen(path);
    memcpy(path + length, ".auto.png", sizeof(".auto.png"));
    loader->preview_ok = gpsp_state_read_picture(path, loader->preview);
}

static void loader_compose(struct loader *loader)
{
    uint16_t *pixels = loader->pixels;
    const char *help;
    char text[256];
    int y = LOADER_LIST_Y;

    gpsp_ui_backdrop(pixels, loader->frame);
    gpsp_ui_panel(pixels);
    snprintf(text, sizeof(text), "Game List (%u)", (unsigned)loader->count);
    gpsp_ui_text(pixels, LOADER_TEXT_X, LOADER_TITLE_Y, text, 37,
            GPSP_UI_COLOR_TITLE);

    if (!loader->count)
        gpsp_ui_text(pixels, LOADER_TEXT_X, y, "No ROMs on the card", 37,
                GPSP_UI_COLOR_DIM);
    for (size_t i = loader->top;
            i < loader->count && i < loader->top + LOADER_ROWS;
            i++, y += GPSP_UI_LINE)
    {
        if (i == loader->cursor)
            gpsp_ui_tint(pixels, GPSP_UI_PANEL_X + 4, y - 2,
                    LOADER_LIST_RIGHT + 4 - GPSP_UI_PANEL_X - 4, GPSP_UI_LINE,
                    GPSP_UI_COLOR_CURSOR);
        loader_title(loader->roms[i], text, sizeof(text));
        gpsp_ui_text_clip(pixels, LOADER_TEXT_X, y, text,
                i == loader->cursor ? loader->marquee.offset : 0,
                LOADER_LIST_WIDTH, GPSP_UI_COLOR_TEXT);
    }
    /* Marks for more names above or below, between list and picture. */
    if (loader->top)
        gpsp_ui_text(pixels, LOADER_LIST_RIGHT + 1, LOADER_LIST_Y, "^", 1,
                GPSP_UI_COLOR_DIM);
    if (loader->top + LOADER_ROWS < loader->count)
        gpsp_ui_text(pixels, LOADER_LIST_RIGHT + 1,
                LOADER_LIST_Y + (LOADER_ROWS - 1) * GPSP_UI_LINE, "v", 1,
                GPSP_UI_COLOR_DIM);

    if (loader->count)
    {
        const int x0 = LOADER_PREVIEW_X;
        const int y0 = LOADER_LIST_Y - 2;
        char title[256];

        loader_read_preview(loader);
        if (loader->preview_ok)
            gpsp_ui_half_frame(pixels, x0, y0, loader->preview);
        else
        {
            gpsp_ui_fill(pixels, x0, y0, LOADER_PREVIEW_W, LOADER_PREVIEW_H,
                    GPSP_UI_COLOR_EMPTY);
            gpsp_ui_text(pixels, x0 + (LOADER_PREVIEW_W - 12 *
                    GPSP_UI_FONT_WIDTH) / 2, y0 + (LOADER_PREVIEW_H -
                    GPSP_UI_FONT_HEIGHT) / 2, "No auto save", 12,
                    GPSP_UI_COLOR_DIM);
        }
        gpsp_ui_frame(pixels, x0 - 1, y0 - 1, LOADER_PREVIEW_W + 2,
                LOADER_PREVIEW_H + 2, GPSP_UI_COLOR_BORDER);

        /* The whole name under the picture, wrapped at spaces. */
        loader_title(loader->roms[loader->cursor], title, sizeof(title));
        const char *rest = title;

        for (int line = 0; line < LOADER_NAME_LINES && *rest; line++)
        {
            char part[LOADER_NAME_CHARS + 1];
            size_t take = strlen(rest);
            int y = y0 + LOADER_PREVIEW_H + 4 + line * GPSP_UI_LINE;

            /* The last line takes the rest and ends in "..." if cut. */
            if (line == LOADER_NAME_LINES - 1 || take <= LOADER_NAME_CHARS)
            {
                gpsp_ui_text(pixels, x0, y, rest, LOADER_NAME_CHARS,
                        GPSP_UI_COLOR_TEXT);
                break;
            }
            take = LOADER_NAME_CHARS;
            for (size_t i = LOADER_NAME_CHARS; i > 0; i--)
                if (rest[i] == ' ')
                {
                    take = i;
                    break;
                }
            memcpy(part, rest, take);
            part[take] = '\0';
            gpsp_ui_text(pixels, x0, y, part, LOADER_NAME_CHARS,
                    GPSP_UI_COLOR_TEXT);
            rest += take;
            while (*rest == ' ')
                rest++;
        }
    }

    help = gpsp_settings_auto_load() ? "A: Play  Sel+A: Fresh  B: Back" :
            "A: Play  B: Back";
    gpsp_ui_text(pixels, LOADER_TEXT_X, LOADER_FOOTER_Y, help, 37,
            GPSP_UI_COLOR_DIM);
}

static void loader_draw(struct loader *loader)
{
    loader_compose(loader);
    gpsp_board_ui_draw(0, GPSP_UI_HEIGHT);
}

/* Scrolls a long cursor name; only its row goes to the display. */
static void loader_scroll(struct loader *loader, int64_t now_us)
{
    char text[256];
    int y;

    if (!loader->count)
        return;
    loader_title(loader->roms[loader->cursor], text, sizeof(text));
    if (!gpsp_ui_marquee_step(&loader->marquee, gpsp_ui_text_width(text),
            LOADER_LIST_WIDTH, now_us))
        return;
    loader_compose(loader);
    y = LOADER_LIST_Y + (int)(loader->cursor - loader->top) * GPSP_UI_LINE;
    gpsp_board_ui_draw(y - 2, y - 2 + GPSP_UI_LINE);
}

static void loader_move(struct loader *loader, uint32_t buttons)
{
    size_t page = LOADER_ROWS;
    size_t cursor = loader->cursor;

    if (!loader->count)
        return;
    if (buttons & LOADER_BUTTON(UP))
        cursor = cursor ? cursor - 1 : loader->count - 1;
    else if (buttons & LOADER_BUTTON(DOWN))
        cursor = cursor + 1 < loader->count ? cursor + 1 : 0;
    else if (buttons & (LOADER_BUTTON(LEFT) | LOADER_BUTTON(L)))
        cursor = cursor > page ? cursor - page : 0;
    else if (buttons & (LOADER_BUTTON(RIGHT) | LOADER_BUTTON(R)))
        cursor = cursor + page < loader->count ? cursor + page :
                loader->count - 1;
    loader->cursor = cursor;
    gpsp_ui_marquee_reset(&loader->marquee, esp_timer_get_time());
    if (cursor < loader->top)
        loader->top = cursor;
    if (cursor >= loader->top + LOADER_ROWS)
        loader->top = cursor - LOADER_ROWS + 1;
    loader_draw(loader);
}

/* The cursor starts on this name: the running ROM, else the last one. */
static void loader_place_cursor(struct loader *loader)
{
    const char *current = gpsp_storage_rom_path();
    char last[256];
    const char *name = NULL;

    if (current)
        name = loader_base(current);
    else if (gpsp_settings_last_rom(last, sizeof(last)))
        name = last;
    for (size_t i = 0; name && i < loader->count; i++)
        if (!strcmp(loader_base(loader->roms[i]), name))
        {
            loader->cursor = i;
            break;
        }
    if (loader->cursor >= LOADER_ROWS)
        loader->top = loader->cursor - LOADER_ROWS / 2;
    if (loader->count > LOADER_ROWS &&
            loader->top > loader->count - LOADER_ROWS)
        loader->top = loader->count - LOADER_ROWS;
}

enum gpsp_loader_result gpsp_loader_run(uint16_t *pixels,
        const uint16_t *frame,
        char *path, size_t size, bool *skip_auto_load)
{
    const uint32_t moves = LOADER_BUTTON(UP) | LOADER_BUTTON(DOWN) |
            LOADER_BUTTON(LEFT) | LOADER_BUTTON(RIGHT) | LOADER_BUTTON(L) |
            LOADER_BUTTON(R);
    struct loader loader = {
        .pixels = pixels,
        .frame = frame,
        .preview_rom = SIZE_MAX,
    };
    uint32_t held;
    uint32_t repeat_buttons = 0;
    int64_t repeat_us = 0;
    enum gpsp_loader_result result = GPSP_LOADER_CANCEL;

    loader.roms = heap_caps_malloc(LOADER_MAX_ROMS * sizeof(*loader.roms),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    loader.preview = heap_caps_malloc(
            GPSP_UI_FRAME_WIDTH * GPSP_UI_FRAME_HEIGHT * 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (loader.roms)
        gpsp_storage_find_roms(loader_add, &loader);
    qsort(loader.roms, loader.count, sizeof(*loader.roms), loader_compare);
    loader_place_cursor(&loader);
    gpsp_ui_marquee_reset(&loader.marquee, esp_timer_get_time());
    loader_draw(&loader);

    /* Buttons held when the list opens are not presses. */
    held = gpsp_board_input_read();
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(LOADER_POLL_MS));
        uint32_t input = gpsp_board_input_read();
        uint32_t pressed = input & ~held;
        int64_t now_us = esp_timer_get_time();
        held = input;
        if (gpsp_ui_poll_command(pixels))
        {
            result = GPSP_LOADER_QUIT;
            break;
        }
        if (pressed & GPSP_BOARD_INPUT_HOME)
        {
            result = GPSP_LOADER_RESUME;
            break;
        }
        if (pressed & LOADER_BUTTON(B))
            break;
        if ((pressed & LOADER_BUTTON(A)) && loader.count)
        {
            snprintf(path, size, "%s", loader.roms[loader.cursor]);
            *skip_auto_load = (input & LOADER_BUTTON(SELECT)) != 0;
            result = GPSP_LOADER_PLAY;
            break;
        }
        repeat_buttons &= input;
        if (pressed & moves)
        {
            repeat_buttons = pressed & moves;
            loader_move(&loader, repeat_buttons);
            repeat_us = now_us + LOADER_REPEAT_DELAY_US;
        }
        else if (repeat_buttons && now_us >= repeat_us)
        {
            loader_move(&loader, repeat_buttons);
            repeat_us = now_us + LOADER_REPEAT_US;
        }
        else
            loader_scroll(&loader, now_us);
    }

    for (size_t i = 0; i < loader.count; i++)
        heap_caps_free(loader.roms[i]);
    heap_caps_free(loader.roms);
    heap_caps_free(loader.preview);
    return result;
}

#endif
