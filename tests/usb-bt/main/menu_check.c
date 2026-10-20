/* Exercise the production menu and settings with modeled peripherals.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#define CONFIG_GPSP_MENU 1
#include "../../../main/gpsp_menu.c"

static int volume = -60, slot = GPSP_STATE_AUTO;
static bool muted;
static char saved_mute[8], saved_volume[8];
static unsigned writes;

int gpsp_board_audio_volume_step(int step)
{
    volume += step * 3;
    if (volume > 0) volume = 0;
    if (volume < -60) volume = -60;
    return volume;
}
bool gpsp_board_audio_muted(void) { return muted; }
void gpsp_board_audio_set_muted(bool value) { muted = value; }
void gpsp_board_video_frameskip(int frameskip) { (void)frameskip; }
bool gpsp_board_display_options(enum gpsp_display_filter *filter,
        enum gpsp_display_color *color)
{
    *filter = GPSP_DISPLAY_NEAREST;
    *color = GPSP_DISPLAY_COLOR_NONE;
    return false;
}
void gpsp_board_set_display_options(enum gpsp_display_filter filter,
        enum gpsp_display_color color) { (void)filter; (void)color; }
bool gpsp_board_stats(void) { return false; }
void gpsp_board_set_stats(bool enabled) { (void)enabled; }
void gpsp_board_ui_draw(unsigned y0, unsigned y1) { (void)y0; (void)y1; }
int gpsp_state_slot(void) { return slot; }
void gpsp_state_select(int value) { slot = value; }
bool gpsp_state_used(int value) { (void)value; return false; }
bool gpsp_state_picture(int value, uint16_t *pixels)
{
    (void)value; (void)pixels; return false;
}
bool gpsp_storage_config(const char *key, char *value, size_t size)
{
    const char *saved = !strcmp(key, "mute") ? saved_mute :
            !strcmp(key, "volume") ? saved_volume : "";
    snprintf(value, size, "%s", saved);
    return saved[0] != '\0';
}
bool gpsp_storage_config_set(const struct gpsp_storage_setting *items, size_t count)
{
    for (unsigned i = 0; i < count; i++)
    {
        char *saved = !strcmp(items[i].key, "mute") ? saved_mute :
                !strcmp(items[i].key, "volume") ? saved_volume : NULL;
        assert(saved);
        snprintf(saved, 8, "%s", items[i].value);
    }
    writes++;
    return true;
}

static int item_index(enum menu_item item)
{
    for (int i = 0; i < s_item_count; i++)
        if (s_items[i] == item)
            return i;
    assert(false);
    return 0;
}

void bt_menu_check(void)
{
    s_pixels = heap_caps_calloc(GPSP_UI_WIDTH * GPSP_UI_HEIGHT, 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(s_pixels);
    gpsp_settings_load();
    gpsp_settings_save();
    assert(!writes && !muted);
    s_in_game = true;
    s_page = MENU_PAGE_SETTINGS;
    menu_build_items();
    item_index(MENU_ITEM_BT);
    s_cursor = item_index(MENU_ITEM_MUTE);
    menu_direction(MENU_BUTTON(RIGHT));
    assert(muted && volume == -60);
    gpsp_settings_save();
    assert(writes == 1 && !strcmp(saved_mute, "on"));
    s_cursor = item_index(MENU_ITEM_VOLUME);
    menu_direction(MENU_BUTTON(RIGHT));
    assert(muted && volume == -57);
    gpsp_settings_save();
    assert(writes == 2 && !strcmp(saved_volume, "-57"));
    muted = false;
    volume = -60;
    gpsp_settings_load();
    assert(muted && volume == -57);
    s_cursor = item_index(MENU_ITEM_MUTE);
    menu_direction(MENU_BUTTON(LEFT));
    assert(!muted && volume == -57);
    gpsp_settings_save();
    assert(writes == 3 && !strcmp(saved_mute, "off"));
    gpsp_settings_save();
    assert(writes == 3);
    puts("USB_BT_CHECK mute menu/persistence/volume: pass");

    s_page = MENU_PAGE_BT;
    gpsp_bt_get_info(&s_bt);
    assert(s_bt.count >= 2);
    s_bt_selected = 0;
    s_cursor = 0;
    s_top = 0;
    menu_build_items();
    assert(s_item_count == 9);
    char value[40];
    for (int i = 0; i < s_item_count; i++)
        if (menu_item_value(s_items[i], value, sizeof(value)))
            assert(gpsp_ui_text_width(value) <= (MENU_WIDE_RIGHT - MENU_TEXT_X) / 2);
    s_cursor = item_index(MENU_ITEM_BT_DEVICE);
    menu_direction(MENU_BUTTON(RIGHT));
    assert(s_bt_selected == 1);
    menu_direction(MENU_BUTTON(LEFT));
    assert(s_bt_selected == 0);
    menu_bt_action(MENU_ITEM_BT_FORGET);
    assert(s_bt_confirm == MENU_ITEM_BT_FORGET);
    menu_direction(MENU_BUTTON(DOWN));
    assert(s_bt_confirm < 0);
    menu_bt_action(MENU_ITEM_BT_FORGET_ALL);
    assert(s_bt_confirm == MENU_ITEM_BT_FORGET_ALL);
    s_bt_poll_us = 0;
    menu_bt_poll(esp_timer_get_time() + 6000000);
    assert(s_bt_confirm < 0);
    puts("USB_BT_CHECK Bluetooth menu/selection/confirmation/layout: pass");
    heap_caps_free(s_pixels);
    s_pixels = NULL;
}
