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

int gpsp_board_audio_volume_step(int step) { (void)step; return INT_MIN; }
void gpsp_board_video_frameskip(int frameskip) { (void)frameskip; }
void gpsp_board_ui_draw(unsigned y0, unsigned y1)
{
    gpsp_lcd_ui_draw(y0, y1);
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
            !strcmp(key, "display_color") ? saved_color : "";

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
        else
            assert(false);
    }
    config_writes++;
    return true;
}

void display_settings_check(uint16_t *ui)
{
    char value[40];
    int display_index = -1, color_index = -1;

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
    }
    assert(display_index >= 0 && color_index >= 0);
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

    display_available = false;
    menu_build_items();
    for (int i = 0; i < s_item_count; i++)
        assert(s_items[i] != MENU_ITEM_DISPLAY && s_items[i] != MENU_ITEM_COLOR);
    gpsp_settings_load();
    gpsp_settings_save();
    assert(config_writes == 1);
    assert(!strcmp(saved_filter, "bilinear") && !strcmp(saved_color, "gba"));
    display_available = true;
    snprintf(saved_filter, sizeof(saved_filter), "bad");
    snprintf(saved_color, sizeof(saved_color), "bad");
    gpsp_settings_load();
    assert(gpsp_settings_display_filter() == GPSP_DISPLAY_NEAREST);
    assert(gpsp_settings_display_color() == GPSP_DISPLAY_COLOR_NONE);
    puts("DSI_CHECK settings/menu pass");
}
