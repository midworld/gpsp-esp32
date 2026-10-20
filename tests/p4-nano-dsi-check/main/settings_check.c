/* Menu edits and settings persistence with an in-memory config store.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include "gpsp_lcd_dsi.h"
#include "gpsp_lcd.h"
/* Exercise the real menu item list and directional input. */
#include "../../../main/gpsp_menu.c"

static bool display_available = true;
static char saved_filter[16];
static char saved_color[16];
static char saved_stats[16];
static char saved_pad[8];
static char saved_opacity[4];
static bool touch_available, virtual_pad;
static unsigned config_writes;
static bool save_ok = true;
static int state_slot = GPSP_STATE_AUTO;

bool gpsp_board_display_options(enum gpsp_display_filter *filter,
        enum gpsp_display_color *color)
{
    gpsp_dsi_options(filter, color);
    return display_available;
}

void gpsp_board_set_display_options(enum gpsp_display_filter filter,
        enum gpsp_display_color color)
{
    if (display_available)
        gpsp_dsi_set_options(filter, color);
}

enum gpsp_display_stats gpsp_board_stats(void) { return gpsp_dsi_stats_mode(); }
void gpsp_board_set_stats(enum gpsp_display_stats mode) { gpsp_dsi_set_stats(mode); }
bool gpsp_board_virtual_pad_available(void) { return touch_available; }
bool gpsp_board_virtual_pad_enabled(void) { return virtual_pad; }
void gpsp_board_set_virtual_pad(bool enabled) { virtual_pad = touch_available && enabled; }
unsigned gpsp_board_virtual_pad_opacity(void) { return gpsp_dsi_pad_opacity(); }
void gpsp_board_set_virtual_pad_opacity(unsigned percent) { gpsp_dsi_set_pad_opacity(percent); }

int gpsp_board_audio_volume_step(int step) { (void)step; return INT_MIN; }
bool gpsp_board_audio_muted(void) { return false; }
void gpsp_board_audio_set_muted(bool muted) { (void)muted; }
void gpsp_board_video_frameskip(int frameskip) { (void)frameskip; }
void gpsp_board_ui_draw(unsigned y0, unsigned y1)
{
    gpsp_lcd_ui_draw(y0, y1);
}
uint32_t *gpsp_board_ui_layer(uint16_t *pixels, uint16_t **background)
{
    return gpsp_lcd_ui_layer(pixels, background);
}
int gpsp_state_slot(void) { return state_slot; }
void gpsp_state_select(int slot) { state_slot = slot; }
bool gpsp_state_used(int slot) { (void)slot; return false; }
bool gpsp_state_picture(int slot, uint16_t *pixels)
{
    (void)slot;
    (void)pixels;
    return false;
}

bool gpsp_storage_config(const char *key, char *value, size_t size)
{
    const char *saved = !strcmp(key, "display_filter") ? saved_filter :
            !strcmp(key, "display_color") ? saved_color :
            !strcmp(key, "display_stats") ? saved_stats :
            !strcmp(key, "virtual_pad") ? saved_pad :
            !strcmp(key, "virtual_pad_opacity") ? saved_opacity : "";

    snprintf(value, size, "%s", saved);
    return saved[0] != '\0';
}

bool gpsp_storage_config_set(const struct gpsp_storage_setting *items, size_t count)
{
    if (!save_ok)
        return false;
    for (size_t i = 0; i < count; i++)
    {
        if (!strcmp(items[i].key, "display_filter"))
            snprintf(saved_filter, sizeof(saved_filter), "%s", items[i].value);
        else if (!strcmp(items[i].key, "display_color"))
            snprintf(saved_color, sizeof(saved_color), "%s", items[i].value);
        else if (!strcmp(items[i].key, "display_stats"))
            snprintf(saved_stats, sizeof(saved_stats), "%s", items[i].value);
        else if (!strcmp(items[i].key, "virtual_pad"))
            snprintf(saved_pad, sizeof(saved_pad), "%s", items[i].value);
        else if (!strcmp(items[i].key, "virtual_pad_opacity"))
            snprintf(saved_opacity, sizeof(saved_opacity), "%s", items[i].value);
        else
            assert(false);
    }
    config_writes++;
    return true;
}

void display_settings_check(uint16_t *ui)
{
    char value[40];
    int display_index = -1, color_index = -1, stats_index = -1;

    gpsp_settings_load();
    assert(gpsp_settings_display_filter() == GPSP_DISPLAY_NEAREST);
    assert(gpsp_settings_display_color() == GPSP_DISPLAY_COLOR_NONE);
    gpsp_settings_save();
    assert(config_writes == 0);
    s_page = MENU_PAGE_SETTINGS;
    s_pixels = ui;
    s_in_game = true;
    menu_build_items();
    for (int i = 0; i < s_item_count; i++)
    {
        if (s_items[i] == MENU_ITEM_DISPLAY)
            display_index = i;
        if (s_items[i] == MENU_ITEM_COLOR)
            color_index = i;
        if (s_items[i] == MENU_ITEM_STATS)
            stats_index = i;
    }
    assert(display_index >= 0 && color_index >= 0 && stats_index >= 0);
    assert(!strcmp(s_item_names[MENU_ITEM_STATS], "Stats"));
    s_cursor = display_index;
    menu_direction(MENU_BUTTON(LEFT));
    assert(gpsp_settings_display_filter() == GPSP_DISPLAY_LCD_GRID);
    menu_direction(MENU_BUTTON(RIGHT));
    assert(gpsp_settings_display_filter() == GPSP_DISPLAY_NEAREST);
    menu_direction(MENU_BUTTON(RIGHT));
    assert(gpsp_settings_display_filter() == GPSP_DISPLAY_BILINEAR);
    assert(menu_item_value(MENU_ITEM_DISPLAY, value, sizeof(value)));
    assert(!strcmp(value, "< 3x Bilinear >"));
    assert(gpsp_ui_text_width(value) <= (MENU_WIDE_RIGHT - MENU_TEXT_X) / 2);
    s_cursor = color_index;
    menu_direction(MENU_BUTTON(RIGHT));
    assert(gpsp_settings_display_color() == GPSP_DISPLAY_COLOR_GBA);
    assert(menu_item_value(MENU_ITEM_COLOR, value, sizeof(value)));
    assert(!strcmp(value, "< GBA LCD >"));

    save_ok = false;
    gpsp_settings_save();
    assert(config_writes == 0);
    save_ok = true;
    gpsp_settings_save();
    assert(config_writes == 1);
    assert(!strcmp(saved_filter, "bilinear") && !strcmp(saved_color, "gba"));
    gpsp_settings_save();
    assert(config_writes == 1);
    gpsp_settings_set_display(GPSP_DISPLAY_NEAREST, GPSP_DISPLAY_COLOR_NONE);
    gpsp_settings_load();
    assert(gpsp_settings_display_filter() == GPSP_DISPLAY_BILINEAR);
    assert(gpsp_settings_display_color() == GPSP_DISPLAY_COLOR_GBA);

    s_cursor = stats_index;
    menu_direction(MENU_BUTTON(RIGHT));
    assert(gpsp_settings_stats() == GPSP_DISPLAY_STATS_SIMPLE);
    assert(menu_item_value(MENU_ITEM_STATS, value, sizeof(value)));
    assert(!strcmp(value, "< Simple >"));
    gpsp_settings_save();
    assert(!strcmp(saved_stats, "simple"));
    gpsp_settings_set_stats(GPSP_DISPLAY_STATS_OFF);
    gpsp_settings_load();
    assert(gpsp_settings_stats() == GPSP_DISPLAY_STATS_SIMPLE);
    menu_direction(MENU_BUTTON(RIGHT));
    assert(gpsp_settings_stats() == GPSP_DISPLAY_STATS_FULL);
    gpsp_settings_save();
    assert(!strcmp(saved_stats, "full"));
    gpsp_settings_load();
    assert(gpsp_settings_stats() == GPSP_DISPLAY_STATS_FULL);
    menu_direction(MENU_BUTTON(RIGHT));
    assert(gpsp_settings_stats() == GPSP_DISPLAY_STATS_OFF);
    menu_direction(MENU_BUTTON(LEFT));
    assert(gpsp_settings_stats() == GPSP_DISPLAY_STATS_FULL);
    menu_direction(MENU_BUTTON(LEFT));
    assert(gpsp_settings_stats() == GPSP_DISPLAY_STATS_SIMPLE);
    menu_direction(MENU_BUTTON(LEFT));
    assert(gpsp_settings_stats() == GPSP_DISPLAY_STATS_OFF);
    gpsp_settings_save();
    assert(!strcmp(saved_stats, "off"));
    snprintf(saved_stats, sizeof(saved_stats), "on");
    gpsp_settings_load();
    assert(gpsp_settings_stats() == GPSP_DISPLAY_STATS_FULL);
    unsigned stats_writes = config_writes;

    display_available = false;
    menu_build_items();
    for (int i = 0; i < s_item_count; i++)
        assert(s_items[i] != MENU_ITEM_DISPLAY && s_items[i] != MENU_ITEM_COLOR &&
                s_items[i] != MENU_ITEM_STATS);
    gpsp_settings_load();
    gpsp_settings_save();
    assert(config_writes == stats_writes);
    assert(!strcmp(saved_filter, "bilinear") && !strcmp(saved_color, "gba"));
    display_available = true;
    snprintf(saved_filter, sizeof(saved_filter), "bad");
    snprintf(saved_color, sizeof(saved_color), "bad");
    snprintf(saved_stats, sizeof(saved_stats), "bad");
    gpsp_settings_load();
    assert(gpsp_settings_display_filter() == GPSP_DISPLAY_NEAREST);
    assert(gpsp_settings_display_color() == GPSP_DISPLAY_COLOR_NONE);
    assert(gpsp_settings_stats() == GPSP_DISPLAY_STATS_OFF);
    touch_available = true;
    menu_build_items();
    int pad_index = -1;
    for (int i = 0; i < s_item_count; i++)
        if (s_items[i] == MENU_ITEM_VIRTUAL_PAD)
            pad_index = i;
    assert(pad_index >= 0);
    s_cursor = pad_index;
    menu_direction(MENU_BUTTON(RIGHT));
    assert(virtual_pad);
    assert(menu_item_value(MENU_ITEM_VIRTUAL_PAD, value, sizeof(value)));
    assert(!strcmp(value, "< On >"));
    gpsp_settings_save();
    assert(!strcmp(saved_pad, "on"));
    virtual_pad = false;
    gpsp_settings_load();
    assert(virtual_pad);
    menu_direction(MENU_BUTTON(LEFT));
    assert(!virtual_pad);
    gpsp_settings_save();
    assert(!strcmp(saved_pad, "off"));
    snprintf(saved_pad, sizeof(saved_pad), "invalid");
    gpsp_settings_load();
    assert(!virtual_pad);
    int opacity_index = -1;
    for (int i = 0; i < s_item_count; i++)
        if (s_items[i] == MENU_ITEM_PAD_OPACITY)
            opacity_index = i;
    assert(opacity_index >= 0 && gpsp_board_virtual_pad_opacity() == 25);
    s_cursor = opacity_index;
    menu_direction(MENU_BUTTON(RIGHT));
    assert(gpsp_board_virtual_pad_opacity() == 50);
    menu_direction(MENU_BUTTON(RIGHT));
    assert(gpsp_board_virtual_pad_opacity() == 100);
    menu_direction(MENU_BUTTON(RIGHT));
    assert(gpsp_board_virtual_pad_opacity() == 25);
    menu_direction(MENU_BUTTON(LEFT));
    assert(gpsp_board_virtual_pad_opacity() == 100);
    assert(menu_item_value(MENU_ITEM_PAD_OPACITY, value, sizeof(value)));
    assert(!strcmp(value, "< 100% >"));
    gpsp_settings_save();
    assert(!strcmp(saved_opacity, "100"));
    gpsp_board_set_virtual_pad_opacity(25);
    gpsp_settings_load();
    assert(gpsp_board_virtual_pad_opacity() == 100);
    snprintf(saved_opacity, sizeof(saved_opacity), "37");
    gpsp_settings_load();
    assert(gpsp_board_virtual_pad_opacity() == 25);
    touch_available = false;
    menu_build_items();
    for (int i = 0; i < s_item_count; i++)
        assert(s_items[i] != MENU_ITEM_VIRTUAL_PAD && s_items[i] != MENU_ITEM_PAD_OPACITY);
    puts("DSI_CHECK settings/menu and Virtual Pad persistence pass");
}
