/* Screen keyboard: type text with the pad.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_keyboard.h"

#include "sdkconfig.h"

#if CONFIG_GPSP_MENU

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "gpsp_board.h"
#include "gpsp_ui.h"
#include "libretro.h"

#define KEYBOARD_POLL_MS 16
#define KEYBOARD_REPEAT_DELAY_US 300000
#define KEYBOARD_REPEAT_US 80000
#define KEYBOARD_TEXT_MAX 64

#define KEYBOARD_COLS 10
#define KEYBOARD_ROWS 4
#define KEYBOARD_CELL_W 26
#define KEYBOARD_CELL_H 20
#define KEYBOARD_X \
    (GPSP_UI_PANEL_X + (GPSP_UI_PANEL_W - KEYBOARD_COLS * KEYBOARD_CELL_W) / 2)
#define KEYBOARD_TEXT_X (GPSP_UI_PANEL_X + 10)
#define KEYBOARD_TITLE_Y (GPSP_UI_PANEL_Y + 8)
#define KEYBOARD_FIELD_Y (KEYBOARD_TITLE_Y + GPSP_UI_LINE + 4)
#define KEYBOARD_GRID_Y (KEYBOARD_FIELD_Y + GPSP_UI_LINE + 12)
/* The function keys sit in a fifth row. */
#define KEYBOARD_FN_ROW KEYBOARD_ROWS
#define KEYBOARD_HELP_Y \
    (GPSP_UI_PANEL_Y + GPSP_UI_PANEL_H - 8 - 2 * GPSP_UI_LINE)
#define KEYBOARD_FIELD_CHARS ((GPSP_UI_PANEL_W - 28) / GPSP_UI_FONT_WIDTH)

#define KEYBOARD_BUTTON(id) (1u << RETRO_DEVICE_ID_JOYPAD_##id)

enum keyboard_page
{
    KEYBOARD_LOWER,
    KEYBOARD_UPPER,
    KEYBOARD_SYMBOLS,
    KEYBOARD_PAGES,
};

/* Every printable character but space is on a page; ' ' is no key. */
static const char s_keys[KEYBOARD_PAGES][KEYBOARD_ROWS][KEYBOARD_COLS + 1] = {
    [KEYBOARD_LOWER] = { "1234567890", "qwertyuiop", "asdfghjkl-",
            "zxcvbnm_.@" },
    [KEYBOARD_UPPER] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL-",
            "ZXCVBNM_.@" },
    [KEYBOARD_SYMBOLS] = { "!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?",
            "`~        " },
};

enum keyboard_fn
{
    KEYBOARD_FN_SHIFT,
    KEYBOARD_FN_SYMBOLS,
    KEYBOARD_FN_SPACE,
    KEYBOARD_FN_DELETE,
    KEYBOARD_FN_OK,
    KEYBOARD_FN_COUNT,
};

/* Function keys: first column, width in columns, label. */
static const struct
{
    int col;
    int span;
    const char *label;
} s_fn_keys[KEYBOARD_FN_COUNT] = {
    [KEYBOARD_FN_SHIFT] = { 0, 2, "Shift" },
    [KEYBOARD_FN_SYMBOLS] = { 2, 2, "Sym" },
    [KEYBOARD_FN_SPACE] = { 4, 3, "Space" },
    [KEYBOARD_FN_DELETE] = { 7, 2, "Del" },
    [KEYBOARD_FN_OK] = { 9, 1, "OK" },
};

struct keyboard
{
    uint16_t *pixels;
    const uint16_t *frame;
    const char *title;
    char text[KEYBOARD_TEXT_MAX + 1];
    size_t max;
    bool secret;
    bool shown;
    enum keyboard_page page;
    int row;
    int col;
};

static int keyboard_fn_at(int col)
{
    for (int i = 0; i < KEYBOARD_FN_COUNT; i++)
        if (col < s_fn_keys[i].col + s_fn_keys[i].span)
            return i;
    return KEYBOARD_FN_OK;
}

static void keyboard_key(struct keyboard *keyboard, int x, int y, int w,
        const char *label, bool on)
{
    uint16_t *pixels = keyboard->pixels;
    int length = (int)strlen(label);

    if (on)
        gpsp_ui_tint(pixels, x + 1, y + 1, w - 2, KEYBOARD_CELL_H - 2,
                GPSP_UI_COLOR_CURSOR);
    gpsp_ui_frame(pixels, x, y, w, KEYBOARD_CELL_H, GPSP_UI_COLOR_BORDER);
    gpsp_ui_text(pixels, x + (w - length * GPSP_UI_FONT_WIDTH) / 2,
            y + (KEYBOARD_CELL_H - GPSP_UI_FONT_HEIGHT) / 2, label, length,
            on ? GPSP_UI_COLOR_TITLE : GPSP_UI_COLOR_TEXT);
}

static void keyboard_draw(struct keyboard *keyboard)
{
    uint16_t *pixels = keyboard->pixels;
    char field[KEYBOARD_TEXT_MAX + 2];
    size_t length = strlen(keyboard->text);
    const char *visible;

    gpsp_ui_backdrop(pixels, keyboard->frame);
    gpsp_ui_panel(pixels);
    gpsp_ui_text(pixels, KEYBOARD_TEXT_X, KEYBOARD_TITLE_Y, keyboard->title,
            37, GPSP_UI_COLOR_TITLE);

    /* The field ends in a cursor and shows the end of long text. */
    for (size_t i = 0; i < length; i++)
        field[i] = keyboard->secret && !keyboard->shown ? '*' :
                keyboard->text[i];
    field[length] = '_';
    field[length + 1] = '\0';
    visible = length + 1 > KEYBOARD_FIELD_CHARS ?
            field + length + 1 - KEYBOARD_FIELD_CHARS : field;
    gpsp_ui_fill(pixels, KEYBOARD_TEXT_X - 2, KEYBOARD_FIELD_Y - 3,
            GPSP_UI_PANEL_W - 16, GPSP_UI_LINE + 2, GPSP_UI_COLOR_EMPTY);
    gpsp_ui_text(pixels, KEYBOARD_TEXT_X + 2, KEYBOARD_FIELD_Y, visible,
            KEYBOARD_FIELD_CHARS, GPSP_UI_COLOR_TITLE);

    for (int row = 0; row < KEYBOARD_ROWS; row++)
        for (int col = 0; col < KEYBOARD_COLS; col++)
        {
            char label[2] = { s_keys[keyboard->page][row][col], '\0' };

            if (label[0] == ' ')
                continue;
            keyboard_key(keyboard, KEYBOARD_X + col * KEYBOARD_CELL_W,
                    KEYBOARD_GRID_Y + row * KEYBOARD_CELL_H, KEYBOARD_CELL_W,
                    label, row == keyboard->row && col == keyboard->col);
        }
    for (int i = 0; i < KEYBOARD_FN_COUNT; i++)
    {
        const char *label = s_fn_keys[i].label;

        if (i == KEYBOARD_FN_SYMBOLS && keyboard->page == KEYBOARD_SYMBOLS)
            label = "abc";
        keyboard_key(keyboard, KEYBOARD_X + s_fn_keys[i].col * KEYBOARD_CELL_W,
                KEYBOARD_GRID_Y + KEYBOARD_FN_ROW * KEYBOARD_CELL_H,
                s_fn_keys[i].span * KEYBOARD_CELL_W, label,
                keyboard->row == KEYBOARD_FN_ROW &&
                keyboard_fn_at(keyboard->col) == i);
    }

    gpsp_ui_text(pixels, KEYBOARD_TEXT_X, KEYBOARD_HELP_Y,
            keyboard->secret ? "Select: Cancel  X: Show" : "Select: Cancel",
            37, GPSP_UI_COLOR_DIM);
    gpsp_ui_text(pixels, KEYBOARD_TEXT_X, KEYBOARD_HELP_Y + GPSP_UI_LINE,
            "A: Type  B: Del  L/R: Page  Start: OK", 37, GPSP_UI_COLOR_DIM);
    gpsp_board_ui_draw(0, GPSP_UI_HEIGHT);
}

static void keyboard_type(struct keyboard *keyboard, char c)
{
    size_t length = strlen(keyboard->text);

    if (length >= keyboard->max)
        return;
    keyboard->text[length] = c;
    keyboard->text[length + 1] = '\0';
}

static void keyboard_delete(struct keyboard *keyboard)
{
    size_t length = strlen(keyboard->text);

    if (length)
        keyboard->text[length - 1] = '\0';
}

/* Moves within the grid; empty symbol cells are passed over. */
static void keyboard_move(struct keyboard *keyboard, uint32_t buttons)
{
    const int rows = KEYBOARD_ROWS + 1;

    for (int tries = 0; tries < KEYBOARD_COLS; tries++)
    {
        if (buttons & KEYBOARD_BUTTON(UP))
            keyboard->row = (keyboard->row + rows - 1) % rows;
        else if (buttons & KEYBOARD_BUTTON(DOWN))
            keyboard->row = (keyboard->row + 1) % rows;
        else if (keyboard->row == KEYBOARD_FN_ROW)
        {
            /* Function keys move a whole key at a time. */
            int fn = keyboard_fn_at(keyboard->col);

            fn = (fn + (buttons & KEYBOARD_BUTTON(LEFT) ?
                    KEYBOARD_FN_COUNT - 1 : 1)) % KEYBOARD_FN_COUNT;
            keyboard->col = s_fn_keys[fn].col;
        }
        else if (buttons & KEYBOARD_BUTTON(LEFT))
            keyboard->col = (keyboard->col + KEYBOARD_COLS - 1) % KEYBOARD_COLS;
        else if (buttons & KEYBOARD_BUTTON(RIGHT))
            keyboard->col = (keyboard->col + 1) % KEYBOARD_COLS;
        if (keyboard->row == KEYBOARD_FN_ROW ||
                s_keys[keyboard->page][keyboard->row][keyboard->col] != ' ')
            break;
    }
    keyboard_draw(keyboard);
}

static void keyboard_set_page(struct keyboard *keyboard,
        enum keyboard_page page)
{
    keyboard->page = page;
    /* The cursor leaves a cell that this page leaves empty. */
    while (keyboard->row < KEYBOARD_ROWS && keyboard->col &&
            s_keys[page][keyboard->row][keyboard->col] == ' ')
        keyboard->col--;
}

/* Returns true when the key ends the editing. */
static bool keyboard_press(struct keyboard *keyboard)
{
    if (keyboard->row < KEYBOARD_ROWS)
    {
        keyboard_type(keyboard,
                s_keys[keyboard->page][keyboard->row][keyboard->col]);
        return false;
    }
    switch (keyboard_fn_at(keyboard->col))
    {
        case KEYBOARD_FN_SHIFT:
            keyboard_set_page(keyboard, keyboard->page == KEYBOARD_LOWER ?
                    KEYBOARD_UPPER : KEYBOARD_LOWER);
            break;
        case KEYBOARD_FN_SYMBOLS:
            keyboard_set_page(keyboard, keyboard->page == KEYBOARD_SYMBOLS ?
                    KEYBOARD_LOWER : KEYBOARD_SYMBOLS);
            break;
        case KEYBOARD_FN_SPACE:
            keyboard_type(keyboard, ' ');
            break;
        case KEYBOARD_FN_DELETE:
            keyboard_delete(keyboard);
            break;
        default:
            return true;
    }
    return false;
}

enum gpsp_ui_exit gpsp_keyboard_run(uint16_t *pixels, const uint16_t *frame,
        const char *title, char *text, size_t size, bool secret)
{
    const uint32_t moves = KEYBOARD_BUTTON(UP) | KEYBOARD_BUTTON(DOWN) |
            KEYBOARD_BUTTON(LEFT) | KEYBOARD_BUTTON(RIGHT);
    struct keyboard keyboard = {
        .pixels = pixels,
        .frame = frame,
        .title = title,
        .max = size - 1 < KEYBOARD_TEXT_MAX ? size - 1 : KEYBOARD_TEXT_MAX,
        .secret = secret,
        .row = 1,
    };
    uint32_t held;
    uint32_t repeat_buttons = 0;
    int64_t repeat_us = 0;
    enum gpsp_ui_exit exit = GPSP_UI_BACK;

    /* Text the keys cannot type is left out. */
    for (const char *c = text; *c; c++)
        if (*c >= ' ' && *c <= '~')
            keyboard_type(&keyboard, *c);
    keyboard_draw(&keyboard);

    held = gpsp_board_input_read();
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(KEYBOARD_POLL_MS));
        uint32_t input = gpsp_board_input_read();
        uint32_t pressed = input & ~held;
        int64_t now_us = esp_timer_get_time();
        bool changed = false;

        held = input;
        if (gpsp_ui_poll_command(pixels))
        {
            exit = GPSP_UI_QUIT;
            break;
        }
        if (pressed & GPSP_BOARD_INPUT_HOME)
        {
            exit = GPSP_UI_RESUME;
            break;
        }
        if (pressed & KEYBOARD_BUTTON(SELECT))
            break;
        if (pressed & KEYBOARD_BUTTON(START))
        {
            exit = GPSP_UI_DONE;
            break;
        }
        if (pressed & KEYBOARD_BUTTON(A))
        {
            if (keyboard_press(&keyboard))
            {
                exit = GPSP_UI_DONE;
                break;
            }
            changed = true;
        }
        if (pressed & KEYBOARD_BUTTON(B))
        {
            keyboard_delete(&keyboard);
            changed = true;
        }
        if (pressed & (KEYBOARD_BUTTON(L) | KEYBOARD_BUTTON(R)))
        {
            int step = pressed & KEYBOARD_BUTTON(R) ? 1 : KEYBOARD_PAGES - 1;

            keyboard_set_page(&keyboard,
                    (enum keyboard_page)((keyboard.page + step) % KEYBOARD_PAGES));
            changed = true;
        }
        if ((pressed & KEYBOARD_BUTTON(X)) && secret)
        {
            keyboard.shown = !keyboard.shown;
            changed = true;
        }
        if (changed)
            keyboard_draw(&keyboard);

        repeat_buttons &= input;
        if (pressed & moves)
        {
            repeat_buttons = pressed & moves;
            keyboard_move(&keyboard, repeat_buttons);
            repeat_us = now_us + KEYBOARD_REPEAT_DELAY_US;
        }
        else if (repeat_buttons && now_us >= repeat_us)
        {
            keyboard_move(&keyboard, repeat_buttons);
            repeat_us = now_us + KEYBOARD_REPEAT_US;
        }
    }

    if (exit == GPSP_UI_DONE)
        memcpy(text, keyboard.text, strlen(keyboard.text) + 1);
    return exit;
}

#endif
