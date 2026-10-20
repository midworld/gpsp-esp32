/* In-game menu over the paused game.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_menu.h"

#include "sdkconfig.h"

#if CONFIG_GPSP_MENU

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "gpsp_board.h"
#include "gpsp_bt.h"
#include "gpsp_keyboard.h"
#include "gpsp_loader.h"
#include "gpsp_screenshot.h"
#include "gpsp_settings.h"
#include "gpsp_state.h"
#include "gpsp_storage.h"
#include "gpsp_transfer_ui.h"
#include "gpsp_ui.h"
#include "gpsp_wifi_ui.h"
#include "libretro.h"

_Static_assert(GPSP_BOARD_UI_WIDTH == GPSP_UI_WIDTH &&
        GPSP_BOARD_UI_HEIGHT == GPSP_UI_HEIGHT, "UI picture size");

#define MENU_FRAME_WIDTH GPSP_UI_FRAME_WIDTH
#define MENU_FRAME_HEIGHT GPSP_UI_FRAME_HEIGHT
#define MENU_POLL_MS 16
/* A held direction repeats after this delay, at this interval. */
#define MENU_REPEAT_DELAY_US 300000
#define MENU_REPEAT_US 80000

#define MENU_PANEL_X GPSP_UI_PANEL_X
#define MENU_PANEL_Y GPSP_UI_PANEL_Y
#define MENU_PANEL_W GPSP_UI_PANEL_W
#define MENU_PANEL_H GPSP_UI_PANEL_H
#define MENU_TEXT_X (MENU_PANEL_X + 10)
#define MENU_TEXT_CHARS ((MENU_PANEL_W - 20) / GPSP_UI_FONT_WIDTH)
#define MENU_LINE GPSP_UI_LINE
#define MENU_LIST_Y (MENU_PANEL_Y + 8 + 2 * MENU_LINE + 8)
/* On the main page item values end here; the slot picture sits right of
   the list. Other pages use the whole panel. */
#define MENU_LIST_RIGHT 160
#define MENU_WIDE_RIGHT (MENU_PANEL_X + MENU_PANEL_W - 10)
#define MENU_DEPTH 3
#define MENU_PREVIEW_X 170
#define MENU_PREVIEW_W (MENU_FRAME_WIDTH / 2)
#define MENU_PREVIEW_H (MENU_FRAME_HEIGHT / 2)
#define MENU_FOOTER_Y (MENU_PANEL_Y + MENU_PANEL_H - 8 - GPSP_UI_FONT_HEIGHT)
#define MENU_ROWS ((MENU_FOOTER_Y - 4 - MENU_LIST_Y) / MENU_LINE)
#define MENU_THRESHOLD_STEP 10
/* A control waits this long for its new button. */
#define MENU_CAPTURE_US 5000000

#define MENU_COLOR_BORDER GPSP_UI_COLOR_BORDER
#define MENU_COLOR_TITLE GPSP_UI_COLOR_TITLE
#define MENU_COLOR_TEXT GPSP_UI_COLOR_TEXT
#define MENU_COLOR_DIM GPSP_UI_COLOR_DIM
#define MENU_COLOR_CURSOR GPSP_UI_COLOR_CURSOR
#define MENU_COLOR_EMPTY GPSP_UI_COLOR_EMPTY

#define MENU_BUTTON(id) (1u << RETRO_DEVICE_ID_JOYPAD_##id)

#define MENU_TITLE "gpSP (RV32 JIT by MK)"

enum menu_page
{
    MENU_PAGE_MAIN,
    MENU_PAGE_SETTINGS,
    MENU_PAGE_CONTROLS,
    MENU_PAGE_WIFI,
    MENU_PAGE_BT,
};

static const char *const s_page_paths[] = {
    [MENU_PAGE_SETTINGS] = "Settings",
    [MENU_PAGE_CONTROLS] = "Settings > Controls",
    [MENU_PAGE_WIFI] = "Settings > Wi-Fi",
    [MENU_PAGE_BT] = "Settings > Bluetooth",
};

/* Items in page order; each page starts at its first item below. */
enum menu_item
{
    MENU_ITEM_RESUME,
    MENU_ITEM_SLOT,
    MENU_ITEM_SAVE,
    MENU_ITEM_LOAD,
    MENU_ITEM_RESET,
    MENU_ITEM_GAME_LIST,
    MENU_ITEM_TRANSFER,
    MENU_ITEM_SETTINGS,
    MENU_ITEM_FRAMESKIP,
    MENU_ITEM_THRESHOLD,
    MENU_ITEM_VOLUME,
    MENU_ITEM_MUTE,
    MENU_ITEM_DISPLAY,
    MENU_ITEM_COLOR,
    MENU_ITEM_STATS,
    MENU_ITEM_VIRTUAL_PAD,
    MENU_ITEM_PAD_OPACITY,
    MENU_ITEM_AUTO_SAVE,
    MENU_ITEM_AUTO_LOAD,
    MENU_ITEM_CONTROLS,
    MENU_ITEM_WIFI,
    MENU_ITEM_BT,
    /* In the order of enum gpsp_settings_control. */
    MENU_ITEM_CONTROL_A,
    MENU_ITEM_CONTROL_B,
    MENU_ITEM_CONTROL_L,
    MENU_ITEM_CONTROL_R,
    MENU_ITEM_CONTROL_START,
    MENU_ITEM_CONTROL_SELECT,
    MENU_ITEM_CONTROL_TURBO_A,
    MENU_ITEM_CONTROL_TURBO_B,
    MENU_ITEM_CONTROL_FAST_FORWARD,
    MENU_ITEM_FAST_FORWARD_SPEED,
    MENU_ITEM_FAST_FORWARD_MODE,
    MENU_ITEM_CONTROLS_RESET,
    MENU_ITEM_WIFI_NETWORK,
    MENU_ITEM_WIFI_PASSWORD,
    MENU_ITEM_WIFI_CONNECT,
    MENU_ITEM_WIFI_FORGET,
    MENU_ITEM_BT_STATUS,
    MENU_ITEM_BT_SEARCH,
    MENU_ITEM_BT_DEVICE,
    MENU_ITEM_BT_ADDRESS,
    MENU_ITEM_BT_CONNECT,
    MENU_ITEM_BT_DISCONNECT,
    MENU_ITEM_BT_AUTO,
    MENU_ITEM_BT_FORGET,
    MENU_ITEM_BT_FORGET_ALL,
    MENU_ITEM_COUNT,
};

static const char *const s_item_names[MENU_ITEM_COUNT] = {
    [MENU_ITEM_RESUME] = "Resume",
    [MENU_ITEM_SLOT] = "Slot",
    [MENU_ITEM_SAVE] = "Save State",
    [MENU_ITEM_LOAD] = "Load State",
    [MENU_ITEM_RESET] = "Reset Game",
    [MENU_ITEM_GAME_LIST] = "Game List",
    [MENU_ITEM_FRAMESKIP] = "Frameskip",
    [MENU_ITEM_THRESHOLD] = "Threshold",
    [MENU_ITEM_VOLUME] = "Volume",
    [MENU_ITEM_MUTE] = "Mute",
    [MENU_ITEM_DISPLAY] = "Display",
    [MENU_ITEM_COLOR] = "Colors",
    [MENU_ITEM_STATS] = "Stats",
    [MENU_ITEM_VIRTUAL_PAD] = "Virtual Pad",
    [MENU_ITEM_PAD_OPACITY] = "Pad Opacity",
    [MENU_ITEM_AUTO_SAVE] = "Auto Save",
    [MENU_ITEM_AUTO_LOAD] = "Auto Load",
    [MENU_ITEM_TRANSFER] = "File Transfer",
    [MENU_ITEM_SETTINGS] = "Settings",
    [MENU_ITEM_CONTROLS] = "Controls",
    [MENU_ITEM_WIFI] = "Wi-Fi",
    [MENU_ITEM_BT] = "Bluetooth",
    [MENU_ITEM_CONTROL_A] = "A",
    [MENU_ITEM_CONTROL_B] = "B",
    [MENU_ITEM_CONTROL_L] = "L",
    [MENU_ITEM_CONTROL_R] = "R",
    [MENU_ITEM_CONTROL_START] = "Start",
    [MENU_ITEM_CONTROL_SELECT] = "Select",
    [MENU_ITEM_CONTROL_TURBO_A] = "Turbo A",
    [MENU_ITEM_CONTROL_TURBO_B] = "Turbo B",
    [MENU_ITEM_CONTROL_FAST_FORWARD] = "Fast Forward",
    [MENU_ITEM_FAST_FORWARD_SPEED] = "FF Speed",
    [MENU_ITEM_FAST_FORWARD_MODE] = "FF Mode",
    [MENU_ITEM_CONTROLS_RESET] = "Reset Controls",
    [MENU_ITEM_WIFI_NETWORK] = "Network",
    [MENU_ITEM_WIFI_PASSWORD] = "Password",
    [MENU_ITEM_WIFI_CONNECT] = "Connect",
    [MENU_ITEM_WIFI_FORGET] = "Forget",
    [MENU_ITEM_BT_STATUS] = "Status",
    [MENU_ITEM_BT_SEARCH] = "Search",
    [MENU_ITEM_BT_DEVICE] = "Controller",
    [MENU_ITEM_BT_ADDRESS] = "Address",
    [MENU_ITEM_BT_CONNECT] = "Pair / Connect",
    [MENU_ITEM_BT_DISCONNECT] = "Disconnect / Cancel",
    [MENU_ITEM_BT_AUTO] = "Auto Reconnect",
    [MENU_ITEM_BT_FORGET] = "Forget Controller",
    [MENU_ITEM_BT_FORGET_ALL] = "Forget All",
};

static enum menu_page menu_item_page(enum menu_item item)
{
    if (item >= MENU_ITEM_BT_STATUS)
        return MENU_PAGE_BT;
    if (item >= MENU_ITEM_WIFI_NETWORK)
        return MENU_PAGE_WIFI;
    if (item >= MENU_ITEM_CONTROL_A)
        return MENU_PAGE_CONTROLS;
    return item >= MENU_ITEM_FRAMESKIP ? MENU_PAGE_SETTINGS : MENU_PAGE_MAIN;
}

enum menu_preview
{
    MENU_PREVIEW_STALE,
    MENU_PREVIEW_EMPTY,
    MENU_PREVIEW_NO_PICTURE,
    MENU_PREVIEW_PICTURE,
};

static uint16_t *s_pixels;
static const uint16_t *s_frame;
/* The items shown, in order: some need hardware the board lacks, the
   game list needs ROMs on the card, and Threshold shows only with
   automatic frameskip. */
static enum menu_item s_items[MENU_ITEM_COUNT];
static int s_item_count;
static int s_cursor;
/* The first item shown; the list scrolls to keep the cursor in view. */
static int s_top;
/* The page shown, and the pages it was opened from. */
static enum menu_page s_page;
static struct
{
    enum menu_page page;
    int cursor;
    int top;
} s_stack[MENU_DEPTH];
static int s_depth;
/* Wi-Fi runs only while its page is open. The page edits a network and
   password; gpsp.cfg gets them only once they connect. */
static atomic_bool s_wifi_open;
static struct gpsp_settings_wifi s_wifi;
static struct gpsp_settings_wifi s_wifi_saved;
static char s_wifi_status[24];

/* A connection runs in its own task, so the menu keeps working. */
enum menu_wifi_job
{
    MENU_WIFI_IDLE,
    MENU_WIFI_BUSY,
    MENU_WIFI_DONE,
};

static atomic_int s_wifi_job;
/* Set when the page closes during a join: the join is dropped. */
static atomic_bool s_wifi_cancel;
static esp_err_t s_wifi_result;
static struct gpsp_settings_wifi s_wifi_try;
static char s_wifi_psk[65];
static char s_wifi_address[16];
static bool s_game_list;
/* File Transfer serves the SD card. */
static bool s_card;
/* False for the menu at boot, before a game runs. */
static bool s_in_game;
/* The game name scrolls when it is longer than the panel. */
static char s_game_name[256];
static struct gpsp_ui_marquee s_name_marquee;
/* The selected slot's picture, read again when the slot changes. */
static uint16_t *s_preview;
static enum menu_preview s_preview_state;
static char s_message[40];
/* The control waiting for a button, or -1. */
static int s_capture = -1;
static int64_t s_capture_end_us;
static struct gpsp_bt_info s_bt;
static unsigned s_bt_selected;
static int s_bt_confirm = -1;
static int64_t s_bt_poll_us, s_bt_confirm_us;

static const char *menu_bt_state(enum gpsp_bt_state state)
{
    static const char *const names[] = {
        "Disabled", "No adapter", "Starting", "Ready", "Searching",
        "Connecting", "Connected", "Disconnecting", "Error",
    };
    return state <= GPSP_BT_ERROR ? names[state] : "Error";
}


/* The ROM file name without its directory and extension. */
static void menu_game_name(char *name, size_t size)
{
    const char *path = gpsp_storage_rom_path();
    const char *base;
    const char *dot;
    size_t length;

    if (!path)
    {
        snprintf(name, size, "No game loaded");
        return;
    }
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    dot = strrchr(base, '.');
    length = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    if (length >= size)
        length = size - 1;
    memcpy(name, base, length);
    name[length] = '\0';
}

/* The value shown right of an item, if it has one. */
static bool menu_item_value(enum menu_item item, char *value, size_t size)
{
    int slot = gpsp_state_slot();

    if (item == MENU_ITEM_VOLUME)
        snprintf(value, size, "< %d dB >", gpsp_board_audio_volume_step(0));
    else if (item == MENU_ITEM_MUTE)
        snprintf(value, size, gpsp_board_audio_muted() ? "< On >" : "< Off >");
    else if (item == MENU_ITEM_FRAMESKIP)
    {
        int frameskip = gpsp_settings_frameskip();

        if (frameskip == GPSP_SETTINGS_FRAMESKIP_AUTO)
            snprintf(value, size, "< Auto >");
        else if (frameskip)
            snprintf(value, size, "< %d >", frameskip);
        else
            snprintf(value, size, "< Off >");
    }
    else if (item == MENU_ITEM_THRESHOLD)
        snprintf(value, size, "< %u%% >", gpsp_settings_frameskip_threshold());
    else if (item == MENU_ITEM_AUTO_SAVE)
        snprintf(value, size, gpsp_settings_auto_save() ? "< On >" : "< Off >");
    else if (item == MENU_ITEM_AUTO_LOAD)
        snprintf(value, size, gpsp_settings_auto_load() ? "< On >" : "< Off >");
    else if (item == MENU_ITEM_DISPLAY)
    {
        static const char *const names[] = {
            "3x Nearest", "3x Bilinear", "3x LCD Grid",
        };

        snprintf(value, size, "< %s >", names[gpsp_settings_display_filter()]);
    }
    else if (item == MENU_ITEM_VIRTUAL_PAD)
        snprintf(value, size, gpsp_board_virtual_pad_enabled() ? "< On >" : "< Off >");
    else if (item == MENU_ITEM_PAD_OPACITY)
        snprintf(value, size, "< %u%% >", gpsp_board_virtual_pad_opacity());
    else if (item == MENU_ITEM_COLOR)
        snprintf(value, size, gpsp_settings_display_color() ==
                GPSP_DISPLAY_COLOR_GBA ? "< GBA LCD >" : "< None >");
    else if (item == MENU_ITEM_STATS)
    {
        const char *const names[GPSP_DISPLAY_STATS_COUNT] = {
            "Off", "Simple", "Full",
        };

        snprintf(value, size, "< %s >", names[gpsp_settings_stats()]);
    }
    else if (item == MENU_ITEM_SETTINGS || item == MENU_ITEM_CONTROLS ||
            item == MENU_ITEM_WIFI || item == MENU_ITEM_BT)
        snprintf(value, size, ">");
    else if (item >= MENU_ITEM_CONTROL_A &&
            item <= MENU_ITEM_CONTROL_FAST_FORWARD)
    {
        int control = item - MENU_ITEM_CONTROL_A;

        snprintf(value, size, "%s", control == s_capture ? "Press a button" :
                gpsp_settings_button_name(gpsp_settings_control(control)));
    }
    else if (item == MENU_ITEM_FAST_FORWARD_SPEED)
        snprintf(value, size, "< %ux >", gpsp_settings_fast_forward_speed());
    else if (item == MENU_ITEM_FAST_FORWARD_MODE)
        snprintf(value, size, gpsp_settings_fast_forward_toggle() ?
                "< Toggle >" : "< Hold >");
    else if (item == MENU_ITEM_WIFI_NETWORK)
        snprintf(value, size, "%s", s_wifi.ssid[0] ? s_wifi.ssid : "(none)");
    else if (item == MENU_ITEM_WIFI_PASSWORD)
        /* A kept key does not tell the password's length. */
        snprintf(value, size, "%s", s_wifi.password[0] || s_wifi.psk[0] ?
                "********" : "(none)");
    else if (item == MENU_ITEM_WIFI_CONNECT && s_wifi_status[0])
        snprintf(value, size, "%s", s_wifi_status);
    else if (item == MENU_ITEM_BT_STATUS)
        snprintf(value, size, "%s", menu_bt_state(s_bt.state));
    else if (item == MENU_ITEM_BT_SEARCH)
        snprintf(value, size, s_bt.state == GPSP_BT_SEARCHING ? "< Stop >" : "< Start >");
    else if (item == MENU_ITEM_BT_DEVICE)
    {
        if (!s_bt.count)
            snprintf(value, size, "(none)");
        else
        {
            const struct gpsp_bt_device *device = &s_bt.devices[s_bt_selected];

            snprintf(value, size, "< %u/%u %.10s%s >", s_bt_selected + 1, s_bt.count,
                    device->pad == GPSP_BT_PAD_DS5 ? "Sense" :
                    device->pad == GPSP_BT_PAD_DS4 ? "DS4" :
                    device->name[0] ? device->name : "Pad",
                    device->paired ? "*" : "");
        }
    }
    else if (item == MENU_ITEM_BT_ADDRESS && s_bt.count)
    {
        const uint8_t *a = s_bt.devices[s_bt_selected].address;
        snprintf(value, size, "%02X:%02X:%02X:%02X:%02X:%02X",
                a[0], a[1], a[2], a[3], a[4], a[5]);
    }
    else if (item == MENU_ITEM_BT_AUTO)
        snprintf(value, size, s_bt.auto_connect ? "< On >" : "< Off >");
    else if (item == MENU_ITEM_SLOT && slot == GPSP_STATE_AUTO)
        snprintf(value, size, "< Auto >");
    else if (item == MENU_ITEM_SLOT)
        snprintf(value, size, "< %d >", slot);
    else
        return false;
    return true;
}

static void menu_read_preview(void)
{
    int slot = gpsp_state_slot();

    if (!gpsp_state_used(slot))
        s_preview_state = MENU_PREVIEW_EMPTY;
    else if (s_preview && gpsp_state_picture(slot, s_preview))
        s_preview_state = MENU_PREVIEW_PICTURE;
    else
        s_preview_state = MENU_PREVIEW_NO_PICTURE;
}

/* The selected slot's picture at half size, or what stands for it. */
static void menu_draw_preview(void)
{
    const int x0 = MENU_PREVIEW_X;
    const int y0 = MENU_LIST_Y - 2;
    const char *text = NULL;
    char caption[16];
    int slot = gpsp_state_slot();

    if (s_preview_state == MENU_PREVIEW_STALE)
        menu_read_preview();
    if (s_preview_state == MENU_PREVIEW_PICTURE)
        gpsp_ui_half_frame(s_pixels, x0, y0, s_preview);
    else
    {
        gpsp_ui_fill(s_pixels, x0, y0, MENU_PREVIEW_W, MENU_PREVIEW_H,
                MENU_COLOR_EMPTY);
        text = s_preview_state == MENU_PREVIEW_EMPTY ? "Empty" : "No picture";
        gpsp_ui_text(s_pixels, x0 + (MENU_PREVIEW_W - (int)strlen(text) *
                GPSP_UI_FONT_WIDTH) / 2, y0 + (MENU_PREVIEW_H -
                GPSP_UI_FONT_HEIGHT) / 2, text, 16, MENU_COLOR_DIM);
    }
    gpsp_ui_frame(s_pixels, x0 - 1, y0 - 1, MENU_PREVIEW_W + 2,
            MENU_PREVIEW_H + 2, MENU_COLOR_BORDER);

    if (slot == GPSP_STATE_AUTO)
        snprintf(caption, sizeof(caption), "Auto slot");
    else
        snprintf(caption, sizeof(caption), "Slot %d", slot);
    gpsp_ui_text(s_pixels, x0, y0 + MENU_PREVIEW_H + 4, caption, 16,
            MENU_COLOR_DIM);
}

static void menu_draw_panel(void)
{
    int y = MENU_PANEL_Y + 8;

    gpsp_ui_panel(s_pixels);

    gpsp_ui_text(s_pixels, MENU_TEXT_X, y, MENU_TITLE, MENU_TEXT_CHARS,
            MENU_COLOR_TITLE);
    y += MENU_LINE;
    if (s_page == MENU_PAGE_MAIN)
        gpsp_ui_text_clip(s_pixels, MENU_TEXT_X, y, s_game_name,
                s_name_marquee.offset, MENU_PANEL_W - 20, MENU_COLOR_DIM);
    else
        gpsp_ui_text(s_pixels, MENU_TEXT_X, y, s_page_paths[s_page],
                MENU_TEXT_CHARS, MENU_COLOR_DIM);
    y = MENU_LIST_Y;

    const int right = s_page == MENU_PAGE_MAIN && s_in_game ? MENU_LIST_RIGHT :
            MENU_WIDE_RIGHT;

    for (int i = s_top; i < s_item_count && i < s_top + MENU_ROWS;
            i++, y += MENU_LINE)
    {
        char value[40];
        int width;

        if (i == s_cursor)
            gpsp_ui_tint(s_pixels, MENU_PANEL_X + 4, y - 2,
                    right + 4 - MENU_PANEL_X - 4, MENU_LINE, MENU_COLOR_CURSOR);
        gpsp_ui_text(s_pixels, MENU_TEXT_X, y, s_item_names[s_items[i]],
                MENU_TEXT_CHARS, MENU_COLOR_TEXT);
        if (!menu_item_value(s_items[i], value, sizeof(value)))
            continue;
        /* A long value, such as a network name, is cut on the left half. */
        width = gpsp_ui_text_width(value);
        if (width > (right - MENU_TEXT_X) / 2)
            width = (right - MENU_TEXT_X) / 2;
        gpsp_ui_text_clip(s_pixels, right - width, y, value, 0, width,
                MENU_COLOR_TEXT);
    }

    /* Marks for more items above or below, right of the list. */
    if (s_top)
        gpsp_ui_text(s_pixels, right + 1, MENU_LIST_Y, "^", 1,
                MENU_COLOR_DIM);
    if (s_top + MENU_ROWS < s_item_count)
        gpsp_ui_text(s_pixels, right + 1,
                MENU_LIST_Y + (MENU_ROWS - 1) * MENU_LINE, "v", 1,
                MENU_COLOR_DIM);

    if (s_page == MENU_PAGE_MAIN && s_in_game)
        menu_draw_preview();
    /* A message takes the place of the button help. */
    if (s_message[0])
        gpsp_ui_text(s_pixels, MENU_TEXT_X, MENU_FOOTER_Y, s_message,
                MENU_TEXT_CHARS, MENU_COLOR_TITLE);
    else
        gpsp_ui_text(s_pixels, MENU_TEXT_X, MENU_FOOTER_Y,
                s_page == MENU_PAGE_BT ?
                "*: Saved  Share/Create + PS: Pair" :
                s_page == MENU_PAGE_CONTROLS ?
                "A: Set  X: Clear  B: Back  <>: Change" :
                s_in_game || s_depth ? "A: OK  B: Back  Left/Right: Change" :
                "A: OK  Left/Right: Change", MENU_TEXT_CHARS,
                MENU_COLOR_DIM);
}

static void menu_redraw(void)
{
    gpsp_ui_backdrop(s_pixels, s_frame);
    menu_draw_panel();
    gpsp_board_ui_draw(0, GPSP_UI_HEIGHT);
}

static void menu_scroll_name(int64_t now_us)
{
    const int y = MENU_PANEL_Y + 8 + MENU_LINE;

    if (s_page != MENU_PAGE_MAIN || !gpsp_ui_marquee_step(&s_name_marquee,
            gpsp_ui_text_width(s_game_name), MENU_PANEL_W - 20, now_us))
        return;
    gpsp_ui_backdrop(s_pixels, s_frame);
    menu_draw_panel();
    gpsp_board_ui_draw(y - 2, y - 2 + MENU_LINE);
}

static void menu_build_items(void)
{
    struct gpsp_bt_info bt;
    gpsp_bt_get_info(&bt);
    bool speaker = gpsp_board_audio_volume_step(0) != INT_MIN;
    bool display = gpsp_settings_display_available();
    bool automatic =
            gpsp_settings_frameskip() == GPSP_SETTINGS_FRAMESKIP_AUTO;

    s_item_count = 0;
    for (int i = 0; i < MENU_ITEM_COUNT; i++)
        if (menu_item_page((enum menu_item)i) == s_page &&
                ((i != MENU_ITEM_VOLUME && i != MENU_ITEM_MUTE) || speaker) &&
                (i != MENU_ITEM_BT || bt.state != GPSP_BT_DISABLED) &&
                ((i != MENU_ITEM_DISPLAY && i != MENU_ITEM_COLOR &&
                        i != MENU_ITEM_STATS) || display) &&
                ((i != MENU_ITEM_VIRTUAL_PAD && i != MENU_ITEM_PAD_OPACITY) ||
                        gpsp_board_virtual_pad_available()) &&
                (i != MENU_ITEM_THRESHOLD || automatic) &&
                (i != MENU_ITEM_GAME_LIST || s_game_list) &&
                (i != MENU_ITEM_TRANSFER || s_card) &&
                (s_in_game || i > MENU_ITEM_RESET))
            s_items[s_item_count++] = (enum menu_item)i;
    if (s_cursor >= s_item_count)
        s_cursor = s_item_count - 1;
}

/* Off, then Auto (it needs the speaker queue), then 1 to 9. */
static void menu_step_frameskip(int step)
{
    int order[GPSP_SETTINGS_FRAMESKIP_MAX + 2];
    int count = 0;
    int index = 0;

    order[count++] = 0;
    if (gpsp_board_audio_volume_step(0) != INT_MIN)
        order[count++] = GPSP_SETTINGS_FRAMESKIP_AUTO;
    for (int n = 1; n <= GPSP_SETTINGS_FRAMESKIP_MAX; n++)
        order[count++] = n;
    for (int i = 0; i < count; i++)
        if (order[i] == gpsp_settings_frameskip())
            index = i;
    index = (index + count + step) % count;
    gpsp_settings_set_frameskip(order[index],
            gpsp_settings_frameskip_threshold());
    menu_build_items();
}

static void menu_step_threshold(int step)
{
    int threshold = (int)gpsp_settings_frameskip_threshold() +
            step * MENU_THRESHOLD_STEP;

    if (threshold < GPSP_SETTINGS_THRESHOLD_MIN)
        threshold = GPSP_SETTINGS_THRESHOLD_MIN;
    if (threshold > GPSP_SETTINGS_THRESHOLD_MAX)
        threshold = GPSP_SETTINGS_THRESHOLD_MAX;
    gpsp_settings_set_frameskip(gpsp_settings_frameskip(),
            (unsigned)threshold);
}

/* Moves the cursor or changes the value under it. */
static void menu_direction(uint32_t buttons)
{
    s_bt_confirm = -1;
    if (s_page == MENU_PAGE_BT)
        s_message[0] = '\0';
    if (buttons & (MENU_BUTTON(UP) | MENU_BUTTON(DOWN)))
    {
        s_cursor += buttons & MENU_BUTTON(UP) ? s_item_count - 1 : 1;
        s_cursor %= s_item_count;
        if (s_cursor < s_top)
            s_top = s_cursor;
        if (s_cursor >= s_top + MENU_ROWS)
            s_top = s_cursor - MENU_ROWS + 1;
    }
    else if (s_items[s_cursor] == MENU_ITEM_VOLUME)
        gpsp_board_audio_volume_step(
                buttons & MENU_BUTTON(RIGHT) ? 1 : -1);
    else if (s_items[s_cursor] == MENU_ITEM_MUTE)
        gpsp_board_audio_set_muted(!gpsp_board_audio_muted());
    else if (s_items[s_cursor] == MENU_ITEM_BT_DEVICE && s_bt.count)
        s_bt_selected = (s_bt_selected + (buttons & MENU_BUTTON(RIGHT) ?
                1 : s_bt.count - 1)) % s_bt.count;
    else if (s_items[s_cursor] == MENU_ITEM_BT_AUTO)
        gpsp_bt_request(s_bt.auto_connect ? GPSP_BT_AUTO_OFF : GPSP_BT_AUTO_ON, NULL);
    else if (s_items[s_cursor] == MENU_ITEM_FRAMESKIP)
        menu_step_frameskip(buttons & MENU_BUTTON(RIGHT) ? 1 : -1);
    else if (s_items[s_cursor] == MENU_ITEM_THRESHOLD)
        menu_step_threshold(buttons & MENU_BUTTON(RIGHT) ? 1 : -1);
    else if (s_items[s_cursor] == MENU_ITEM_AUTO_SAVE)
        gpsp_settings_set_auto(!gpsp_settings_auto_save(),
                gpsp_settings_auto_load());
    else if (s_items[s_cursor] == MENU_ITEM_AUTO_LOAD)
        gpsp_settings_set_auto(gpsp_settings_auto_save(),
                !gpsp_settings_auto_load());
    else if (s_items[s_cursor] == MENU_ITEM_DISPLAY)
    {
        unsigned filter = gpsp_settings_display_filter();

        filter += buttons & MENU_BUTTON(RIGHT) ? 1 : GPSP_DISPLAY_FILTER_COUNT - 1;
        gpsp_settings_set_display(filter % GPSP_DISPLAY_FILTER_COUNT,
                gpsp_settings_display_color());
    }
    else if (s_items[s_cursor] == MENU_ITEM_VIRTUAL_PAD)
        gpsp_board_set_virtual_pad(!gpsp_board_virtual_pad_enabled());
    else if (s_items[s_cursor] == MENU_ITEM_PAD_OPACITY)
    {
        unsigned opacity = gpsp_board_virtual_pad_opacity();
        if (buttons & MENU_BUTTON(RIGHT))
            opacity = opacity == 25 ? 50 : opacity == 50 ? 100 : 25;
        else
            opacity = opacity == 100 ? 50 : opacity == 50 ? 25 : 100;
        gpsp_board_set_virtual_pad_opacity(opacity);
    }
    else if (s_items[s_cursor] == MENU_ITEM_COLOR)
        gpsp_settings_set_display(gpsp_settings_display_filter(),
                gpsp_settings_display_color() == GPSP_DISPLAY_COLOR_NONE ?
                GPSP_DISPLAY_COLOR_GBA : GPSP_DISPLAY_COLOR_NONE);
    else if (s_items[s_cursor] == MENU_ITEM_STATS)
    {
        unsigned mode = gpsp_settings_stats();

        mode += buttons & MENU_BUTTON(RIGHT) ? 1 : GPSP_DISPLAY_STATS_COUNT - 1;
        gpsp_settings_set_stats(mode % GPSP_DISPLAY_STATS_COUNT);
    }
    else if (s_items[s_cursor] == MENU_ITEM_FAST_FORWARD_SPEED)
    {
        /* Two speeds: either direction picks the other one. */
        unsigned speed = gpsp_settings_fast_forward_speed() ==
                GPSP_SETTINGS_FAST_FORWARD_MIN ? GPSP_SETTINGS_FAST_FORWARD_MAX :
                GPSP_SETTINGS_FAST_FORWARD_MIN;

        gpsp_settings_set_fast_forward(speed,
                gpsp_settings_fast_forward_toggle());
    }
    else if (s_items[s_cursor] == MENU_ITEM_FAST_FORWARD_MODE)
        gpsp_settings_set_fast_forward(gpsp_settings_fast_forward_speed(),
                !gpsp_settings_fast_forward_toggle());
    else if (s_items[s_cursor] == MENU_ITEM_SLOT ||
            s_items[s_cursor] == MENU_ITEM_SAVE ||
            s_items[s_cursor] == MENU_ITEM_LOAD)
    {
        /* Auto comes before slot 0. */
        int count = GPSP_STATE_SLOTS + 1;
        int index = gpsp_state_slot() - GPSP_STATE_AUTO;

        index += buttons & MENU_BUTTON(RIGHT) ? 1 : count - 1;
        gpsp_state_select(index % count + GPSP_STATE_AUTO);
        s_preview_state = MENU_PREVIEW_STALE;
        s_message[0] = '\0';
    }
    else
        return;
    menu_redraw();
}

static void menu_show(const char *message)
{
    snprintf(s_message, sizeof(s_message), "%s", message);
    menu_redraw();
}

static const char *menu_bt_error(enum gpsp_bt_error error)
{
    switch (error)
    {
        case GPSP_BT_INIT_FAILED: return "Bluetooth initialization failed";
        case GPSP_BT_INIT_TIMEOUT: return "Bluetooth initialization timed out";
        case GPSP_BT_NO_CLASSIC: return "Adapter needs Bluetooth Classic";
        case GPSP_BT_CONNECT_FAILED: return "Connection failed. Try pairing again";
        case GPSP_BT_CONNECT_TIMEOUT: return "Connection timed out. Try again";
        case GPSP_BT_STORAGE_FAILED: return "Could not save Bluetooth settings";
        case GPSP_BT_LAYOUT_FAILED: return "Could not read controller layout";
        case GPSP_BT_UNSUPPORTED: return "Not a supported controller";
        default: return "";
    }
}

static void menu_bt_poll(int64_t now)
{
    if (s_page != MENU_PAGE_BT || now < s_bt_poll_us)
        return;
    s_bt_poll_us = now + 200000;
    struct gpsp_bt_info info;
    gpsp_bt_get_info(&info);
    bool changed = memcmp(&info, &s_bt, sizeof(info)) != 0;
    bool expired = s_bt_confirm >= 0 && now >= s_bt_confirm_us;

    if (!changed && !expired)
        return;
    if (s_bt_selected >= info.count)
        s_bt_selected = 0;
    if (info.state == GPSP_BT_ABSENT || info.state == GPSP_BT_STARTING)
        s_bt_confirm = -1;
    if (expired)
        s_bt_confirm = -1;
    if (s_bt_confirm < 0 && (expired || info.completed != s_bt.completed || info.state != s_bt.state ||
            info.error != s_bt.error))
    {
        s_message[0] = '\0';
        if (info.error)
            snprintf(s_message, sizeof(s_message), "%s", menu_bt_error(info.error));
        else if (info.state == GPSP_BT_ABSENT)
            snprintf(s_message, sizeof(s_message), "Connect a USB Bluetooth adapter");
        else if (info.state == GPSP_BT_SEARCHING)
            snprintf(s_message, sizeof(s_message), "Share/Create + PS, then select a pad");
        else if (info.state == GPSP_BT_CONNECTING)
            snprintf(s_message, sizeof(s_message), "Connecting...");
        else if (info.state == GPSP_BT_CONNECTED)
            snprintf(s_message, sizeof(s_message), "Controller connected");
        else if (s_bt.state == GPSP_BT_SEARCHING && info.state == GPSP_BT_IDLE)
        {
            bool found = false;
            for (unsigned i = 0; i < info.count; i++)
                found |= info.devices[i].found;
            snprintf(s_message, sizeof(s_message), "%s", found ?
                    "Select a pad, then Pair / Connect" : "No controllers found");
        }
        else if (info.completed != s_bt.completed)
            snprintf(s_message, sizeof(s_message), "Done");
    }
    s_bt = info;
    menu_redraw();
}

static void menu_bt_action(enum menu_item item)
{
    enum gpsp_bt_action action;
    const uint8_t *address = s_bt.count ? s_bt.devices[s_bt_selected].address : NULL;

    if (s_bt.state < GPSP_BT_IDLE || s_bt.state > GPSP_BT_CONNECTED)
    {
        menu_show(s_bt.error ? menu_bt_error(s_bt.error) : "Bluetooth adapter is not ready");
        return;
    }
    switch (item)
    {
        case MENU_ITEM_BT_SEARCH:
            if (s_bt.state == GPSP_BT_CONNECTED || s_bt.state == GPSP_BT_CONNECTING)
            {
                menu_show("Disconnect before searching");
                return;
            }
            action = s_bt.state == GPSP_BT_SEARCHING ? GPSP_BT_STOP_SEARCH : GPSP_BT_SEARCH;
            break;
        case MENU_ITEM_BT_CONNECT:
            if (!address)
            {
                menu_show("Search for a controller first");
                return;
            }
            if (s_bt.state == GPSP_BT_CONNECTED || s_bt.state == GPSP_BT_CONNECTING)
            {
                menu_show("Disconnect the current controller first");
                return;
            }
            action = GPSP_BT_CONNECT;
            break;
        case MENU_ITEM_BT_DISCONNECT:
            action = GPSP_BT_DISCONNECT;
            break;
        case MENU_ITEM_BT_AUTO:
            action = s_bt.auto_connect ? GPSP_BT_AUTO_OFF : GPSP_BT_AUTO_ON;
            break;
        case MENU_ITEM_BT_FORGET:
        case MENU_ITEM_BT_FORGET_ALL:
            if (item == MENU_ITEM_BT_FORGET && !address)
                return;
            if (s_bt_confirm != item)
            {
                s_bt_confirm = item;
                s_bt_confirm_us = esp_timer_get_time() + 5000000;
                menu_show(item == MENU_ITEM_BT_FORGET ?
                        "A: Forget this pad?  B: Cancel" : "A: Forget all pads?  B: Cancel");
                return;
            }
            s_bt_confirm = -1;
            action = item == MENU_ITEM_BT_FORGET ? GPSP_BT_FORGET : GPSP_BT_FORGET_ALL;
            break;
        default:
            return;
    }
    menu_show(gpsp_bt_request(action, address) ? "Working..." : "Bluetooth is busy. Try again");
}

static void menu_bt_close(void)
{
    if (s_page == MENU_PAGE_BT)
    {
        struct gpsp_bt_info info;
        gpsp_bt_get_info(&info);
        if (info.state == GPSP_BT_SEARCHING)
            gpsp_bt_request(GPSP_BT_STOP_SEARCH, NULL);
    }
    s_bt_confirm = -1;
}

static void menu_save(void)
{
    bool ok;

    menu_show("Saving...");
    ok = gpsp_state_save(gpsp_state_slot(), s_frame, MENU_FRAME_WIDTH * 2);
    s_preview_state = MENU_PREVIEW_STALE;
    menu_show(ok ? "Saved" : "Save failed");
}

/* Returns true when the state is loaded and the menu can close. */
static bool menu_load(void)
{
    if (!gpsp_state_used(gpsp_state_slot()))
    {
        menu_show("This slot is empty");
        return false;
    }
    menu_show("Loading...");
    if (gpsp_state_load(gpsp_state_slot()))
        return true;
    menu_show("Load failed");
    return false;
}

static void menu_wifi_connect(void);

static void menu_open_page(enum menu_page page)
{
    if (s_depth >= MENU_DEPTH)
        return;
    s_stack[s_depth].page = s_page;
    s_stack[s_depth].cursor = s_cursor;
    s_stack[s_depth].top = s_top;
    s_depth++;
    s_page = page;
    s_cursor = 0;
    s_top = 0;
    s_message[0] = '\0';
    if (page == MENU_PAGE_WIFI)
    {
        gpsp_settings_wifi(&s_wifi_saved);
        s_wifi = s_wifi_saved;
        s_wifi_status[0] = '\0';
    }
    if (page == MENU_PAGE_BT)
    {
        gpsp_bt_get_info(&s_bt);
        s_bt_selected = 0;
        s_bt_confirm = -1;
        s_bt_poll_us = 0;
        for (unsigned i = 0; i < s_bt.count; i++)
            if (!memcmp(s_bt.devices[i].address, s_bt.active, 6))
                s_bt_selected = i;
        snprintf(s_message, sizeof(s_message), "%s", s_bt.error ?
                menu_bt_error(s_bt.error) : s_bt.state == GPSP_BT_ABSENT ?
                "Connect a USB Bluetooth adapter" : "");
    }
    menu_build_items();
    menu_redraw();
    /* A saved network is joined right away. */
    if (page == MENU_PAGE_WIFI && s_wifi.ssid[0])
        menu_wifi_connect();
}

/* Waits for a connection in progress, then stops Wi-Fi. */
static void menu_wifi_close(void)
{
    if (atomic_load(&s_wifi_job) == MENU_WIFI_BUSY)
    {
        /* Startup and any RPC in flight must finish before the radio stops. */
        atomic_store(&s_wifi_cancel, true);
        menu_show("Stopping Wi-Fi...");
        while (atomic_load(&s_wifi_job) == MENU_WIFI_BUSY)
        {
            gpsp_board_wifi_cancel();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    atomic_store(&s_wifi_job, MENU_WIFI_IDLE);
    if (s_wifi_open)
    {
        gpsp_board_wifi_close();
        s_wifi_open = false;
    }
}

/* Returns false on the main page: B then closes the menu. */
static bool menu_close_page(void)
{
    if (!s_depth)
        return false;
    if (s_page == MENU_PAGE_WIFI)
        menu_wifi_close();
    menu_bt_close();
    s_depth--;
    s_page = s_stack[s_depth].page;
    s_cursor = s_stack[s_depth].cursor;
    s_top = s_stack[s_depth].top;
    s_message[0] = '\0';
    menu_build_items();
    menu_redraw();
    return true;
}

/* Wi-Fi starts on the first action that needs it. */
static bool menu_wifi_start(void)
{
    if (s_wifi_open)
        return true;
    menu_show("Starting Wi-Fi...");
    s_wifi_open = gpsp_board_wifi_open() == ESP_OK;
    if (!s_wifi_open)
        menu_show("Wi-Fi did not start");
    return s_wifi_open;
}

static void menu_wifi_task(void *arg)
{
    esp_err_t result = ESP_OK;

    (void)arg;
    if (!s_wifi_open)
    {
        result = gpsp_board_wifi_open();
        s_wifi_open = result == ESP_OK;
    }
    if (result == ESP_OK && atomic_load(&s_wifi_cancel))
        result = ESP_ERR_INVALID_STATE;
    if (result == ESP_OK)
        /* A typed password comes before a kept key. */
        result = gpsp_board_wifi_connect(s_wifi_try.ssid,
                s_wifi_try.password[0] ? s_wifi_try.password : s_wifi_try.psk,
                s_wifi_address, sizeof(s_wifi_address), s_wifi_psk,
                sizeof(s_wifi_psk));
    s_wifi_result = result;
    atomic_store(&s_wifi_job, MENU_WIFI_DONE);
    vTaskDelete(NULL);
}

static void menu_wifi_connect(void)
{
    if (!s_wifi.ssid[0])
    {
        menu_show("Choose a network first");
        return;
    }
    s_wifi_try = s_wifi;
    snprintf(s_wifi_status, sizeof(s_wifi_status), "...");
    atomic_store(&s_wifi_cancel, false);
    atomic_store(&s_wifi_job, MENU_WIFI_BUSY);
    if (xTaskCreate(menu_wifi_task, "gpsp_wifi", 4096, NULL,
            tskIDLE_PRIORITY + 2, NULL) != pdPASS)
    {
        atomic_store(&s_wifi_job, MENU_WIFI_IDLE);
        s_wifi_status[0] = '\0';
        menu_show("Wi-Fi did not start");
        return;
    }
    menu_show(s_wifi_open ? "Connecting..." : "Starting Wi-Fi, connecting...");
}

/* Shows how a connection ended; a network that joins is saved. */
static void menu_wifi_poll(void)
{
    char message[sizeof(s_message)];

    if (atomic_load(&s_wifi_job) != MENU_WIFI_DONE)
        return;
    atomic_store(&s_wifi_job, MENU_WIFI_IDLE);
    if (s_wifi_result == ESP_OK)
    {
        snprintf(s_wifi_status, sizeof(s_wifi_status), "%s", s_wifi_address);
        snprintf(message, sizeof(message), "Connected, IPv4 %s",
                s_wifi_address);
        /* A WPA2 passphrase that joined is kept as its key only. */
        if (s_wifi_psk[0])
        {
            s_wifi_try.password[0] = '\0';
            memcpy(s_wifi_try.psk, s_wifi_psk, sizeof(s_wifi_try.psk));
        }
        else if (s_wifi_try.password[0])
            s_wifi_try.psk[0] = '\0';
        memset(s_wifi_psk, 0, sizeof(s_wifi_psk));
        s_wifi = s_wifi_try;
        if (strcmp(s_wifi_try.ssid, s_wifi_saved.ssid) ||
                strcmp(s_wifi_try.password, s_wifi_saved.password) ||
                strcmp(s_wifi_try.psk, s_wifi_saved.psk))
        {
            if (gpsp_settings_set_wifi(&s_wifi_try))
                s_wifi_saved = s_wifi_try;
            else
                snprintf(message, sizeof(message), "Connected, not saved");
        }
    }
    else
    {
        const char *status = "Failed";
        const char *reason = "Not connected";

        if (!s_wifi_open)
        {
            status = "Off";
            reason = "Wi-Fi did not start";
        }
        else if (s_wifi_result == ESP_ERR_NOT_FOUND)
        {
            status = "Not found";
            reason = "Network not found";
        }
        else if (s_wifi_result == ESP_ERR_INVALID_ARG)
        {
            status = "Refused";
            reason = "Not connected: check the password";
        }
        else if (s_wifi_result == ESP_ERR_TIMEOUT)
        {
            status = "No answer";
            reason = "Not connected: no answer";
        }
        snprintf(s_wifi_status, sizeof(s_wifi_status), "%s", status);
        snprintf(message, sizeof(message), "%s", reason);
    }
    menu_show(message);
}

/* Returns how the screen it opened ended; Home and quit leave the menu. */
static enum gpsp_ui_exit menu_wifi_action(enum menu_item item)
{
    enum gpsp_ui_exit exit = GPSP_UI_DONE;
    char title[48];

    if (atomic_load(&s_wifi_job) == MENU_WIFI_BUSY)
    {
        menu_show("Connecting, please wait");
        return GPSP_UI_BACK;
    }
    /* Once not busy, the worker cannot publish another result. */
    menu_wifi_poll();
    switch (item)
    {
        case MENU_ITEM_WIFI_NETWORK:
        {
            char ssid[sizeof(s_wifi.ssid)];

            if (!menu_wifi_start())
                return GPSP_UI_BACK;
            exit = gpsp_wifi_ui_choose(s_pixels, s_frame, ssid, sizeof(ssid));
            if (exit == GPSP_UI_DONE && strcmp(ssid, s_wifi.ssid))
            {
                /* Another network needs its own password. */
                memcpy(s_wifi.ssid, ssid, sizeof(s_wifi.ssid));
                s_wifi.password[0] = '\0';
                s_wifi.psk[0] = '\0';
                s_wifi_status[0] = '\0';
            }
            break;
        }
        case MENU_ITEM_WIFI_PASSWORD:
            snprintf(title, sizeof(title), "Password for %s",
                    s_wifi.ssid[0] ? s_wifi.ssid : "the network");
            /* A kept key cannot be shown: typing starts over. */
            exit = gpsp_keyboard_run(s_pixels, s_frame, title, s_wifi.password,
                    sizeof(s_wifi.password), true);
            if (exit == GPSP_UI_DONE)
                s_wifi_status[0] = '\0';
            break;
        case MENU_ITEM_WIFI_CONNECT:
            menu_wifi_connect();
            return GPSP_UI_DONE;
        case MENU_ITEM_WIFI_FORGET:
            /* Forgetting is saved at once, and leaves the network. */
            if (s_wifi_open)
            {
                gpsp_board_wifi_close();
                s_wifi_open = false;
            }
            memset(&s_wifi, 0, sizeof(s_wifi));
            s_wifi_status[0] = '\0';
            if (gpsp_settings_set_wifi(&s_wifi))
                s_wifi_saved = s_wifi;
            break;
        default:
            return GPSP_UI_BACK;
    }
    if (exit == GPSP_UI_RESUME || exit == GPSP_UI_QUIT)
        return exit;
    s_message[0] = '\0';
    menu_redraw();
    return exit;
}

#if CONFIG_GPSP_SD_CARD
/* A chosen ROM is asked for; B redraws the menu for more choices. */
static enum gpsp_loader_result menu_game_list(void)
{
    char path[300];
    bool skip_auto_load;
    enum gpsp_loader_result result;

    result = gpsp_loader_run(s_pixels, s_frame, path, sizeof(path),
            &skip_auto_load);
    if (result == GPSP_LOADER_PLAY)
        gpsp_storage_request_rom(path, skip_auto_load);
    else if (result == GPSP_LOADER_CANCEL)
        menu_redraw();
    return result;
}
#endif

/* Gives the waiting control the first button pressed; the D-pad is not
   taken. Home cancels, and so does a wait past MENU_CAPTURE_US. */
static void menu_capture(uint32_t pressed, int64_t now_us)
{
    uint32_t buttons = pressed & 0xffff & ~(MENU_BUTTON(UP) |
            MENU_BUTTON(DOWN) | MENU_BUTTON(LEFT) | MENU_BUTTON(RIGHT));

    if (pressed & GPSP_BOARD_INPUT_HOME || now_us >= s_capture_end_us)
        buttons = 0;
    else if (!buttons)
        return;
    if (buttons)
        gpsp_settings_set_control(s_capture, __builtin_ctz(buttons));
    s_capture = -1;
    s_message[0] = '\0';
    menu_redraw();
}

enum gpsp_menu_result gpsp_menu_run(bool in_game)
{
    const uint32_t directions = MENU_BUTTON(UP) | MENU_BUTTON(DOWN) |
            MENU_BUTTON(LEFT) | MENU_BUTTON(RIGHT);
    uint32_t held;
    uint32_t repeat_buttons = 0;
    int64_t repeat_us = 0;
    enum gpsp_menu_result result = GPSP_MENU_RESUME;

#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
    ESP_LOGI("gpsp_menu", "Menu begin in_game=%d", in_game);
#endif
    if (!gpsp_board_ui_begin(&s_pixels, &s_frame))
        return GPSP_MENU_RESUME;
    s_in_game = in_game;
    gpsp_ui_set_game(in_game);
#if CONFIG_GPSP_SD_CARD
    s_game_list = gpsp_storage_find_roms(NULL, NULL) > 0;
    s_card = gpsp_storage_card();
#endif
    s_cursor = 0;
    s_top = 0;
    s_page = MENU_PAGE_MAIN;
    s_depth = 0;
    menu_build_items();
    menu_game_name(s_game_name, sizeof(s_game_name));
    gpsp_ui_marquee_reset(&s_name_marquee, esp_timer_get_time());
    s_message[0] = '\0';
    /* A command may have closed the last menu during a wait. */
    s_capture = -1;
    if (!s_preview)
        s_preview = heap_caps_malloc(MENU_FRAME_WIDTH * MENU_FRAME_HEIGHT * 2,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_preview_state = MENU_PREVIEW_STALE;
    menu_redraw();
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
    ESP_LOGI("gpsp_menu", "Menu ready");
#endif

    /* Buttons held when the menu opens are not presses. */
    held = gpsp_board_input_read();
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(MENU_POLL_MS));
        uint32_t input = gpsp_board_input_read();
        uint32_t pressed = input & ~held;
        int64_t now_us = esp_timer_get_time();

        held = input;
        menu_bt_poll(now_us);
        /* A press that ends a wait does nothing else; commands below
           still run. */
        if (s_capture >= 0)
        {
            menu_capture(pressed, now_us);
            pressed = 0;
        }
        /* Without a game the menu only leaves through Game List. */
        if ((pressed & GPSP_BOARD_INPUT_HOME) && s_in_game)
            break;
        if (pressed & MENU_BUTTON(B))
        {
            if (s_bt_confirm >= 0)
            {
                s_bt_confirm = -1;
                menu_show("Canceled");
                continue;
            }
            if (!menu_close_page() && s_in_game)
                break;
        }
        if ((pressed & MENU_BUTTON(X)) &&
                s_items[s_cursor] >= MENU_ITEM_CONTROL_A &&
                s_items[s_cursor] <= MENU_ITEM_CONTROL_FAST_FORWARD)
        {
            gpsp_settings_set_control(s_items[s_cursor] - MENU_ITEM_CONTROL_A,
                    GPSP_SETTINGS_BUTTON_NONE);
            menu_redraw();
        }
        if (pressed & MENU_BUTTON(A))
        {
            enum menu_item item = s_items[s_cursor];

            if (item == MENU_ITEM_SETTINGS || item == MENU_ITEM_CONTROLS ||
                    item == MENU_ITEM_WIFI || item == MENU_ITEM_BT)
            {
                menu_open_page(item == MENU_ITEM_SETTINGS ? MENU_PAGE_SETTINGS :
                        item == MENU_ITEM_CONTROLS ? MENU_PAGE_CONTROLS :
                        item == MENU_ITEM_WIFI ? MENU_PAGE_WIFI : MENU_PAGE_BT);
                continue;
            }
            if (item == MENU_ITEM_MUTE)
            {
                gpsp_board_audio_set_muted(!gpsp_board_audio_muted());
                menu_redraw();
                continue;
            }
            if (menu_item_page(item) == MENU_PAGE_BT)
            {
                menu_bt_action(item);
                continue;
            }
            if (item >= MENU_ITEM_CONTROL_A &&
                    item <= MENU_ITEM_CONTROL_FAST_FORWARD)
            {
                s_capture = item - MENU_ITEM_CONTROL_A;
                s_capture_end_us = now_us + MENU_CAPTURE_US;
                repeat_buttons = 0;
                menu_show("Press a button. Home: Cancel");
                continue;
            }
            if (item == MENU_ITEM_CONTROLS_RESET)
            {
                gpsp_settings_reset_controls();
                menu_show("Controls reset");
                continue;
            }
            if (menu_item_page(item) == MENU_PAGE_WIFI)
            {
                enum gpsp_ui_exit exit = menu_wifi_action(item);

                if (exit == GPSP_UI_RESUME)
                {
                    if (s_in_game)
                        break;
                    menu_redraw();
                }
                if (exit == GPSP_UI_QUIT)
                {
                    result = GPSP_MENU_QUIT;
                    break;
                }
                /* The keyboard and list used the buttons still held. */
                held = gpsp_board_input_read();
                repeat_buttons = 0;
                continue;
            }

            if (item == MENU_ITEM_RESET)
                retro_reset();
            if (item == MENU_ITEM_RESUME || item == MENU_ITEM_RESET)
                break;
            if (item == MENU_ITEM_SAVE)
                menu_save();
            if (item == MENU_ITEM_LOAD && menu_load())
                break;
#if CONFIG_GPSP_SD_CARD
            if (item == MENU_ITEM_TRANSFER)
            {
                bool changed;
                enum gpsp_ui_exit exit = gpsp_transfer_ui_run(s_pixels, s_frame,
                        &changed);

                if (exit == GPSP_UI_QUIT)
                {
                    result = changed ? GPSP_MENU_QUIT_KEEP_FILES : GPSP_MENU_QUIT;
                    break;
                }
                /* New files of this game take effect from a fresh start. */
                if (changed)
                {
                    gpsp_storage_request_rom(gpsp_storage_rom_path(), false);
                    result = GPSP_MENU_RELOAD;
                    break;
                }
                if (exit == GPSP_UI_RESUME && s_in_game)
                    break;
                /* Uploads may have brought new ROMs. */
                s_game_list = gpsp_storage_find_roms(NULL, NULL) > 0;
                menu_build_items();
                menu_redraw();
                held = gpsp_board_input_read();
                repeat_buttons = 0;
                continue;
            }
            if (item == MENU_ITEM_GAME_LIST)
            {
                enum gpsp_loader_result chosen = menu_game_list();

                if (chosen == GPSP_LOADER_PLAY)
                    result = GPSP_MENU_SWITCH;
                else if (chosen == GPSP_LOADER_QUIT)
                    result = GPSP_MENU_QUIT;
                /* Home from the list returns to a game, if one runs. */
                if (chosen == GPSP_LOADER_RESUME && !s_in_game)
                    menu_redraw();
                else if (chosen != GPSP_LOADER_CANCEL)
                    break;
                /* The list already consumed the held cancel button. */
                held = gpsp_board_input_read();
                repeat_buttons = 0;
                continue;
            }
#endif
        }
        repeat_buttons &= input;
        if (pressed & directions)
        {
            repeat_buttons = pressed & directions;
            menu_direction(repeat_buttons);
            repeat_us = now_us + MENU_REPEAT_DELAY_US;
        }
        else if (repeat_buttons && now_us >= repeat_us)
        {
            menu_direction(repeat_buttons);
            repeat_us = now_us + MENU_REPEAT_US;
        }
        else
            menu_scroll_name(now_us);
        menu_wifi_poll();
        struct gpsp_board_request request = gpsp_ui_command();

        if (request.command == GPSP_BOARD_COMMAND_QUIT)
        {
            result = GPSP_MENU_QUIT;
            break;
        }
        if (request.command == GPSP_BOARD_COMMAND_SCREENSHOT)
            gpsp_screenshot_print(s_pixels, GPSP_UI_WIDTH, GPSP_UI_HEIGHT,
                    GPSP_UI_WIDTH * 2);
        else if ((request.command == GPSP_BOARD_COMMAND_SAVE_STATE ||
                request.command == GPSP_BOARD_COMMAND_LOAD_STATE) &&
                gpsp_board_command_valid(request) && !s_in_game)
            gpsp_board_command_report(request, false);
        else if ((request.command == GPSP_BOARD_COMMAND_SAVE_STATE ||
                request.command == GPSP_BOARD_COMMAND_LOAD_STATE) &&
                gpsp_board_command_valid(request))
        {
            int slot = gpsp_state_slot();
            bool success = request.command == GPSP_BOARD_COMMAND_SAVE_STATE ?
                    gpsp_state_save(slot, s_frame, MENU_FRAME_WIDTH * 2) :
                    gpsp_state_load(slot);

            gpsp_board_command_report(request, success);
            if (request.command == GPSP_BOARD_COMMAND_LOAD_STATE && success)
                break;
            s_preview_state = MENU_PREVIEW_STALE;
            menu_redraw();
        }
    }

    menu_wifi_close();
    menu_bt_close();
    if (result != GPSP_MENU_RESUME)
        gpsp_ui_set_game(false);
    gpsp_board_ui_end();
    gpsp_settings_save();
#if CONFIG_GPSP_USB_PAD_DIAGNOSTICS
    ESP_LOGI("gpsp_menu", "Menu end result=%d", result);
#endif
    return result;
}

#endif
