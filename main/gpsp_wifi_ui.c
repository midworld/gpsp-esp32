/* Wi-Fi network list for the menu.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_wifi_ui.h"

#include "sdkconfig.h"

#if CONFIG_GPSP_MENU

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "gpsp_board.h"
#include "gpsp_keyboard.h"
#include "gpsp_ui.h"
#include "libretro.h"

#define WIFI_POLL_MS 16
#define WIFI_REPEAT_DELAY_US 300000
#define WIFI_REPEAT_US 80000

#define WIFI_TEXT_X (GPSP_UI_PANEL_X + 10)
#define WIFI_RIGHT (GPSP_UI_PANEL_X + GPSP_UI_PANEL_W - 10)
#define WIFI_TITLE_Y (GPSP_UI_PANEL_Y + 8)
#define WIFI_LIST_Y (WIFI_TITLE_Y + GPSP_UI_LINE + 8)
#define WIFI_FOOTER_Y \
    (GPSP_UI_PANEL_Y + GPSP_UI_PANEL_H - 8 - GPSP_UI_FONT_HEIGHT)
#define WIFI_ROWS ((WIFI_FOOTER_Y - 4 - WIFI_LIST_Y) / GPSP_UI_LINE)
/* Signal and lock take the right of each row. */
#define WIFI_INFO_CHARS 9
#define WIFI_NAME_WIDTH \
    (WIFI_RIGHT - WIFI_TEXT_X - (WIFI_INFO_CHARS + 1) * GPSP_UI_FONT_WIDTH)

#define WIFI_BUTTON(id) (1u << RETRO_DEVICE_ID_JOYPAD_##id)

struct wifi_list
{
    uint16_t *pixels;
    const uint16_t *frame;
    struct gpsp_board_wifi_ap aps[GPSP_BOARD_WIFI_SCAN_MAX];
    /* Rows: the networks found, then one to type a hidden name. */
    int count;
    int cursor;
    int top;
    struct gpsp_ui_marquee marquee;
};

static void wifi_draw_frame(struct wifi_list *list, const char *title)
{
    gpsp_ui_backdrop(list->pixels, list->frame);
    gpsp_ui_panel(list->pixels);
    gpsp_ui_text(list->pixels, WIFI_TEXT_X, WIFI_TITLE_Y, title, 37,
            GPSP_UI_COLOR_TITLE);
}

static void wifi_show_message(struct wifi_list *list, const char *message)
{
    wifi_draw_frame(list, "Wi-Fi Networks");
    gpsp_ui_text(list->pixels, WIFI_TEXT_X, WIFI_LIST_Y, message, 37,
            GPSP_UI_COLOR_TEXT);
    gpsp_board_ui_draw(0, GPSP_UI_HEIGHT);
}

static const char *wifi_row_name(struct wifi_list *list, int row)
{
    return row < list->count ? list->aps[row].ssid : "Other network...";
}

/* Draws the list and sends rows y0 to y1 of the screen. */
static void wifi_draw_rows(struct wifi_list *list, int y0, int y1)
{
    uint16_t *pixels = list->pixels;
    int rows = list->count + 1;
    int y = WIFI_LIST_Y;

    wifi_draw_frame(list, list->count ? "Wi-Fi Networks" :
            "Wi-Fi Networks: none found");
    for (int i = list->top; i < rows && i < list->top + WIFI_ROWS;
            i++, y += GPSP_UI_LINE)
    {
        if (i == list->cursor)
            gpsp_ui_tint(pixels, GPSP_UI_PANEL_X + 4, y - 2,
                    GPSP_UI_PANEL_W - 8, GPSP_UI_LINE, GPSP_UI_COLOR_CURSOR);
        gpsp_ui_text_clip(pixels, WIFI_TEXT_X, y, wifi_row_name(list, i),
                i == list->cursor ? list->marquee.offset : 0, WIFI_NAME_WIDTH,
                i < list->count ? GPSP_UI_COLOR_TEXT : GPSP_UI_COLOR_DIM);
        if (i < list->count)
        {
            char info[WIFI_INFO_CHARS + 1];

            snprintf(info, sizeof(info), "%4d %s", list->aps[i].rssi,
                    list->aps[i].secure ? "Lock" : "Open");
            gpsp_ui_text(pixels, WIFI_RIGHT -
                    (int)strlen(info) * GPSP_UI_FONT_WIDTH, y, info,
                    WIFI_INFO_CHARS, GPSP_UI_COLOR_DIM);
        }
    }
    gpsp_ui_text(pixels, WIFI_TEXT_X, WIFI_FOOTER_Y,
            "A: Choose  B: Back  Y: Scan again", 37, GPSP_UI_COLOR_DIM);
    gpsp_board_ui_draw(y0, y1);
}

static void wifi_draw(struct wifi_list *list)
{
    wifi_draw_rows(list, 0, GPSP_UI_HEIGHT);
}

static void wifi_scan(struct wifi_list *list)
{
    wifi_show_message(list, "Scanning...");
    list->count = (int)gpsp_board_wifi_scan(list->aps,
            GPSP_BOARD_WIFI_SCAN_MAX);
    list->cursor = 0;
    list->top = 0;
    gpsp_ui_marquee_reset(&list->marquee, esp_timer_get_time());
    wifi_draw(list);
}

static void wifi_move(struct wifi_list *list, uint32_t buttons)
{
    int rows = list->count + 1;

    if (buttons & WIFI_BUTTON(UP))
        list->cursor = (list->cursor + rows - 1) % rows;
    else if (buttons & WIFI_BUTTON(DOWN))
        list->cursor = (list->cursor + 1) % rows;
    else
        return;
    if (list->cursor < list->top)
        list->top = list->cursor;
    if (list->cursor >= list->top + WIFI_ROWS)
        list->top = list->cursor - WIFI_ROWS + 1;
    gpsp_ui_marquee_reset(&list->marquee, esp_timer_get_time());
    wifi_draw(list);
}

/* Scrolls a long cursor name; only its row goes to the display. */
static void wifi_scroll(struct wifi_list *list, int64_t now_us)
{
    int y = WIFI_LIST_Y + (list->cursor - list->top) * GPSP_UI_LINE;

    if (gpsp_ui_marquee_step(&list->marquee,
            gpsp_ui_text_width(wifi_row_name(list, list->cursor)),
            WIFI_NAME_WIDTH, now_us))
        wifi_draw_rows(list, y - 2, y - 2 + GPSP_UI_LINE);
}

enum gpsp_ui_exit gpsp_wifi_ui_choose(uint16_t *pixels,
        const uint16_t *frame, char *ssid, size_t size)
{
    static struct wifi_list list;
    uint32_t held;
    uint32_t repeat_buttons = 0;
    int64_t repeat_us = 0;
    enum gpsp_ui_exit exit = GPSP_UI_BACK;

    list.pixels = pixels;
    list.frame = frame;
    wifi_scan(&list);

    held = gpsp_board_input_read();
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(WIFI_POLL_MS));
        uint32_t input = gpsp_board_input_read();
        uint32_t pressed = input & ~held;
        int64_t now_us = esp_timer_get_time();

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
        if (pressed & WIFI_BUTTON(B))
            break;
        if (pressed & WIFI_BUTTON(Y))
        {
            wifi_scan(&list);
            held = gpsp_board_input_read();
            continue;
        }
        if (pressed & WIFI_BUTTON(A))
        {
            if (list.cursor < list.count)
            {
                snprintf(ssid, size, "%s", list.aps[list.cursor].ssid);
                exit = GPSP_UI_DONE;
                break;
            }
            char name[33] = "";

            exit = gpsp_keyboard_run(pixels, frame, "Network name", name,
                    sizeof(name), false);
            if (exit == GPSP_UI_DONE && name[0])
            {
                snprintf(ssid, size, "%s", name);
                break;
            }
            /* Home and quit leave the list too; a cancel returns to it. */
            if (exit == GPSP_UI_RESUME || exit == GPSP_UI_QUIT)
                break;
            exit = GPSP_UI_BACK;
            wifi_draw(&list);
            held = gpsp_board_input_read();
            continue;
        }
        repeat_buttons &= input;
        if (pressed & (WIFI_BUTTON(UP) | WIFI_BUTTON(DOWN)))
        {
            repeat_buttons = pressed & (WIFI_BUTTON(UP) | WIFI_BUTTON(DOWN));
            wifi_move(&list, repeat_buttons);
            repeat_us = now_us + WIFI_REPEAT_DELAY_US;
        }
        else if (repeat_buttons && now_us >= repeat_us)
        {
            wifi_move(&list, repeat_buttons);
            repeat_us = now_us + WIFI_REPEAT_US;
        }
        else
            wifi_scroll(&list, now_us);
    }
    return exit;
}

#endif
