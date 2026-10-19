/* Settings the menu changes, kept in gpsp.cfg:
 *
 *   frameskip=off|auto|<n>      <n> frames skipped after each drawn one
 *   frameskip_threshold=<n>     auto frameskip level, % of the queue target
 *   volume=<dB>                 speaker volume, -60 to 0
 *   state_slot=auto|<n>         state slot, 0 to 9
 *   state_auto_save=on|off      save the auto slot when the game closes
 *   state_auto_load=on|off      load the auto slot when the game starts
 *   button_<control>=<button>   the pad button for a game button, turbo
 *                               or fast forward, such as button_a=b, or
 *                               none
 *   fast_forward_speed=2|3      game frames run for each one shown
 *   fast_forward_mode=hold|toggle
 *   display_filter=nearest|bilinear|lcd_grid
 *   display_color=none|gba      DSI game output only
 *   display_stats=on|off       DSI game statistics overlay
 *   last_rom=<name>             the ROM last started, for the game list
 *   wifi_ssid=<name>            the Wi-Fi network the menu joins
 *   wifi_password=<text>        its password, as plain text
 *   wifi_psk=<hex>              its WPA2 key instead, 64 hex digits
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_settings.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "sdkconfig.h"

#include "gpsp_board.h"
#include "gpsp_state.h"
#include "gpsp_storage.h"
#include "libretro.h"

#define GPSP_SETTINGS_VOLUME_STEP_DB 3

#ifdef CONFIG_GPSP_FRAMESKIP_THRESHOLD
#define GPSP_SETTINGS_THRESHOLD CONFIG_GPSP_FRAMESKIP_THRESHOLD
#else
#define GPSP_SETTINGS_THRESHOLD 50
#endif

struct gpsp_settings
{
    int frameskip;
    unsigned threshold;
    int volume_db;
    int slot;
    bool auto_save;
    bool auto_load;
    unsigned controls[GPSP_SETTINGS_CONTROL_COUNT];
    unsigned fast_forward_speed;
    bool fast_forward_toggle;
    enum gpsp_display_filter display_filter;
    enum gpsp_display_color display_color;
    bool display_stats;
};

static const char *const s_filter_keys[GPSP_DISPLAY_FILTER_COUNT] = {
    "nearest", "bilinear", "lcd_grid",
};
static const char *const s_color_keys[GPSP_DISPLAY_COLOR_COUNT] = {
    "none", "gba",
};

static const char *const s_control_keys[GPSP_SETTINGS_CONTROL_COUNT] = {
    [GPSP_SETTINGS_CONTROL_A] = "button_a",
    [GPSP_SETTINGS_CONTROL_B] = "button_b",
    [GPSP_SETTINGS_CONTROL_L] = "button_l",
    [GPSP_SETTINGS_CONTROL_R] = "button_r",
    [GPSP_SETTINGS_CONTROL_START] = "button_start",
    [GPSP_SETTINGS_CONTROL_SELECT] = "button_select",
    [GPSP_SETTINGS_CONTROL_TURBO_A] = "button_turbo_a",
    [GPSP_SETTINGS_CONTROL_TURBO_B] = "button_turbo_b",
    [GPSP_SETTINGS_CONTROL_FAST_FORWARD] = "button_fast_forward",
};

static const unsigned s_control_defaults[GPSP_SETTINGS_CONTROL_COUNT] = {
    [GPSP_SETTINGS_CONTROL_A] = RETRO_DEVICE_ID_JOYPAD_A,
    [GPSP_SETTINGS_CONTROL_B] = RETRO_DEVICE_ID_JOYPAD_B,
    [GPSP_SETTINGS_CONTROL_L] = RETRO_DEVICE_ID_JOYPAD_L,
    [GPSP_SETTINGS_CONTROL_R] = RETRO_DEVICE_ID_JOYPAD_R,
    [GPSP_SETTINGS_CONTROL_START] = RETRO_DEVICE_ID_JOYPAD_START,
    [GPSP_SETTINGS_CONTROL_SELECT] = RETRO_DEVICE_ID_JOYPAD_SELECT,
    [GPSP_SETTINGS_CONTROL_TURBO_A] = RETRO_DEVICE_ID_JOYPAD_X,
    [GPSP_SETTINGS_CONTROL_TURBO_B] = RETRO_DEVICE_ID_JOYPAD_Y,
    [GPSP_SETTINGS_CONTROL_FAST_FORWARD] = RETRO_DEVICE_ID_JOYPAD_R2,
};

/* Buttons that can be given to a control; the D-pad cannot. Names are
   shown in the menu and, in lower case, kept in gpsp.cfg. */
static const char *const s_button_names[] = {
    [RETRO_DEVICE_ID_JOYPAD_B] = "B",
    [RETRO_DEVICE_ID_JOYPAD_Y] = "Y",
    [RETRO_DEVICE_ID_JOYPAD_SELECT] = "Select",
    [RETRO_DEVICE_ID_JOYPAD_START] = "Start",
    [RETRO_DEVICE_ID_JOYPAD_A] = "A",
    [RETRO_DEVICE_ID_JOYPAD_X] = "X",
    [RETRO_DEVICE_ID_JOYPAD_L] = "L",
    [RETRO_DEVICE_ID_JOYPAD_R] = "R",
    [RETRO_DEVICE_ID_JOYPAD_L2] = "L2",
    [RETRO_DEVICE_ID_JOYPAD_R2] = "R2",
    [RETRO_DEVICE_ID_JOYPAD_L3] = "L3",
    [RETRO_DEVICE_ID_JOYPAD_R3] = "R3",
};

#define GPSP_SETTINGS_BUTTONS \
    (sizeof(s_button_names) / sizeof(s_button_names[0]))

static const char *TAG = "gpsp_settings";
static int s_frameskip;
static unsigned s_threshold = GPSP_SETTINGS_THRESHOLD;
static bool s_core_update;
static bool s_auto_save = true;
static bool s_auto_load;
static unsigned s_controls[GPSP_SETTINGS_CONTROL_COUNT];
static unsigned s_fast_forward_speed = GPSP_SETTINGS_FAST_FORWARD_MIN;
static bool s_fast_forward_toggle;
/* As last read or written, to write only what changed. */
static struct gpsp_settings s_saved;

/* A whole decimal number within a range. */
static bool gpsp_settings_number(const char *text, long min, long max,
        long *number)
{
    char *end;
    long value = strtol(text, &end, 10);

    if (end == text || *end || value < min || value > max)
        return false;
    *number = value;
    return true;
}

static bool gpsp_settings_switch(const char *key, bool *on)
{
    char value[8];

    if (!gpsp_storage_config(key, value, sizeof(value)))
        return false;
    if (!strcmp(value, "on"))
        *on = true;
    else if (!strcmp(value, "off"))
        *on = false;
    else
        return false;
    return true;
}

static void gpsp_settings_current(struct gpsp_settings *settings)
{
    settings->frameskip = s_frameskip;
    settings->threshold = s_threshold;
    settings->volume_db = gpsp_board_audio_volume_step(0);
    settings->slot = gpsp_state_slot();
    settings->auto_save = s_auto_save;
    settings->auto_load = s_auto_load;
    memcpy(settings->controls, s_controls, sizeof(s_controls));
    settings->fast_forward_speed = s_fast_forward_speed;
    settings->fast_forward_toggle = s_fast_forward_toggle;
    gpsp_board_display_options(&settings->display_filter,
            &settings->display_color);
    settings->display_stats = gpsp_board_stats();
}

/* Parse a button name; leave the value unchanged for unknown names. */
static bool gpsp_settings_button(const char *name, unsigned *button)
{
    if (!strcasecmp(name, "none"))
    {
        *button = GPSP_SETTINGS_BUTTON_NONE;
        return true;
    }
    for (unsigned i = 0; i < GPSP_SETTINGS_BUTTONS; i++)
        if (s_button_names[i] && !strcasecmp(name, s_button_names[i]))
        {
            *button = i;
            return true;
        }
    return false;
}

static void gpsp_settings_load_controls(void)
{
    char value[8];
    uint32_t used = 0;

    memcpy(s_controls, s_control_defaults, sizeof(s_controls));
    for (int i = 0; i < GPSP_SETTINGS_CONTROL_COUNT; i++)
        if (gpsp_storage_config(s_control_keys[i], value, sizeof(value)) &&
                !gpsp_settings_button(value, &s_controls[i]))
            ESP_LOGW(TAG, "Unknown button %s=%s; using the default",
                    s_control_keys[i], value);
    for (int i = 0; i < GPSP_SETTINGS_CONTROL_COUNT; i++)
    {
        if (s_controls[i] == GPSP_SETTINGS_BUTTON_NONE)
            continue;
        if (used & (1u << s_controls[i]))
        {
            ESP_LOGW(TAG, "Controls share a button; using the defaults");
            memcpy(s_controls, s_control_defaults, sizeof(s_controls));
            return;
        }
        used |= 1u << s_controls[i];
    }
}

void gpsp_settings_load(void)
{
    char value[16];
    long number;

#if CONFIG_GPSP_FRAMESKIP_AUTO
    s_frameskip = GPSP_SETTINGS_FRAMESKIP_AUTO;
#endif
    if (gpsp_storage_config("frameskip", value, sizeof(value)))
    {
        if (!strcmp(value, "auto"))
            s_frameskip = GPSP_SETTINGS_FRAMESKIP_AUTO;
        else if (gpsp_settings_number(value, 1, GPSP_SETTINGS_FRAMESKIP_MAX,
                &number))
            s_frameskip = (int)number;
        else
            s_frameskip = 0;
    }
    if (gpsp_storage_config("frameskip_threshold", value, sizeof(value)) &&
            gpsp_settings_number(value, GPSP_SETTINGS_THRESHOLD_MIN,
                    GPSP_SETTINGS_THRESHOLD_MAX, &number))
        s_threshold = (unsigned)number;

    int volume_db = gpsp_board_audio_volume_step(0);

    if (volume_db != INT_MIN &&
            gpsp_storage_config("volume", value, sizeof(value)) &&
            gpsp_settings_number(value, -60, 0, &number))
        gpsp_board_audio_volume_step(((int)number - volume_db) /
                GPSP_SETTINGS_VOLUME_STEP_DB);

    if (gpsp_storage_config("state_slot", value, sizeof(value)))
    {
        if (!strcmp(value, "auto"))
            gpsp_state_select(GPSP_STATE_AUTO);
        else if (gpsp_settings_number(value, 0, GPSP_STATE_SLOTS - 1, &number))
            gpsp_state_select((int)number);
    }

    gpsp_settings_switch("state_auto_save", &s_auto_save);
    gpsp_settings_switch("state_auto_load", &s_auto_load);
    gpsp_board_video_frameskip(s_frameskip);

    gpsp_settings_load_controls();
    if (gpsp_storage_config("fast_forward_speed", value, sizeof(value)) &&
            gpsp_settings_number(value, GPSP_SETTINGS_FAST_FORWARD_MIN,
                    GPSP_SETTINGS_FAST_FORWARD_MAX, &number))
        s_fast_forward_speed = (unsigned)number;
    if (gpsp_storage_config("fast_forward_mode", value, sizeof(value)))
        s_fast_forward_toggle = !strcmp(value, "toggle");

    if (gpsp_settings_display_available())
    {
        enum gpsp_display_filter filter = GPSP_DISPLAY_NEAREST;
        enum gpsp_display_color color = GPSP_DISPLAY_COLOR_NONE;

        if (gpsp_storage_config("display_filter", value, sizeof(value)))
            for (unsigned i = 0; i < GPSP_DISPLAY_FILTER_COUNT; i++)
                if (!strcmp(value, s_filter_keys[i]))
                    filter = i;
        if (gpsp_storage_config("display_color", value, sizeof(value)))
            for (unsigned i = 0; i < GPSP_DISPLAY_COLOR_COUNT; i++)
                if (!strcmp(value, s_color_keys[i]))
                    color = i;
        gpsp_settings_set_display(filter, color);
        bool stats = false;
        gpsp_settings_switch("display_stats", &stats);
        gpsp_settings_set_stats(stats);
    }

    gpsp_settings_current(&s_saved);
    ESP_LOGI(TAG, "Settings: frameskip=%d threshold=%u%% volume=%d slot=%d "
            "auto_save=%d auto_load=%d fast_forward=%ux%s", s_saved.frameskip,
            s_saved.threshold, s_saved.volume_db, s_saved.slot,
            s_saved.auto_save, s_saved.auto_load, s_saved.fast_forward_speed,
            s_saved.fast_forward_toggle ? " toggle" : "");
}

void gpsp_settings_save(void)
{
    struct gpsp_storage_setting settings[12 + GPSP_SETTINGS_CONTROL_COUNT];
    char controls[GPSP_SETTINGS_CONTROL_COUNT][8];
    char speed[4];
    char frameskip[8];
    char threshold[8];
    char volume[8];
    char slot[8];
    struct gpsp_settings now;
    size_t count = 0;

    gpsp_settings_current(&now);
    if (now.frameskip != s_saved.frameskip)
    {
        if (now.frameskip == GPSP_SETTINGS_FRAMESKIP_AUTO)
            snprintf(frameskip, sizeof(frameskip), "auto");
        else if (now.frameskip)
            snprintf(frameskip, sizeof(frameskip), "%d", now.frameskip);
        else
            snprintf(frameskip, sizeof(frameskip), "off");
        settings[count++] = (struct gpsp_storage_setting){
            "frameskip", frameskip };
    }
    if (now.threshold != s_saved.threshold)
    {
        snprintf(threshold, sizeof(threshold), "%u", now.threshold);
        settings[count++] = (struct gpsp_storage_setting){
            "frameskip_threshold", threshold };
    }
    if (now.volume_db != s_saved.volume_db)
    {
        snprintf(volume, sizeof(volume), "%d", now.volume_db);
        settings[count++] = (struct gpsp_storage_setting){ "volume", volume };
    }
    if (now.slot != s_saved.slot)
    {
        if (now.slot == GPSP_STATE_AUTO)
            snprintf(slot, sizeof(slot), "auto");
        else
            snprintf(slot, sizeof(slot), "%d", now.slot);
        settings[count++] = (struct gpsp_storage_setting){
            "state_slot", slot };
    }

    if (now.auto_save != s_saved.auto_save)
        settings[count++] = (struct gpsp_storage_setting){
            "state_auto_save", now.auto_save ? "on" : "off" };
    if (now.auto_load != s_saved.auto_load)
        settings[count++] = (struct gpsp_storage_setting){
            "state_auto_load", now.auto_load ? "on" : "off" };

    for (int i = 0; i < GPSP_SETTINGS_CONTROL_COUNT; i++)
    {
        if (now.controls[i] == s_saved.controls[i])
            continue;
        snprintf(controls[i], sizeof(controls[i]), "%s",
                gpsp_settings_button_name(now.controls[i]));
        for (char *c = controls[i]; *c; c++)
            *c = (char)tolower((unsigned char)*c);
        settings[count++] = (struct gpsp_storage_setting){
            s_control_keys[i], controls[i] };
    }
    if (now.fast_forward_speed != s_saved.fast_forward_speed)
    {
        snprintf(speed, sizeof(speed), "%u", now.fast_forward_speed);
        settings[count++] = (struct gpsp_storage_setting){
            "fast_forward_speed", speed };
    }
    if (now.fast_forward_toggle != s_saved.fast_forward_toggle)
        settings[count++] = (struct gpsp_storage_setting){
            "fast_forward_mode", now.fast_forward_toggle ? "toggle" : "hold" };

    if (gpsp_settings_display_available())
    {
        if (now.display_filter != s_saved.display_filter)
            settings[count++] = (struct gpsp_storage_setting){
                "display_filter", s_filter_keys[now.display_filter] };
        if (now.display_color != s_saved.display_color)
            settings[count++] = (struct gpsp_storage_setting){
                "display_color", s_color_keys[now.display_color] };
        if (now.display_stats != s_saved.display_stats)
            settings[count++] = (struct gpsp_storage_setting){
                "display_stats", now.display_stats ? "on" : "off" };
    }

    /* Without a card the settings last until power off. */
    if (count && gpsp_storage_config_set(settings, count))
        s_saved = now;
}

bool gpsp_settings_display_available(void)
{
    enum gpsp_display_filter filter;
    enum gpsp_display_color color;

    return gpsp_board_display_options(&filter, &color);
}

enum gpsp_display_filter gpsp_settings_display_filter(void)
{
    enum gpsp_display_filter filter;
    enum gpsp_display_color color;

    gpsp_board_display_options(&filter, &color);
    return filter;
}

enum gpsp_display_color gpsp_settings_display_color(void)
{
    enum gpsp_display_filter filter;
    enum gpsp_display_color color;

    gpsp_board_display_options(&filter, &color);
    return color;
}

void gpsp_settings_set_display(enum gpsp_display_filter filter,
        enum gpsp_display_color color)
{
    gpsp_board_set_display_options(filter, color);
}

bool gpsp_settings_stats(void)
{
    return gpsp_board_stats();
}

void gpsp_settings_set_stats(bool enabled)
{
    gpsp_board_set_stats(enabled);
}

int gpsp_settings_frameskip(void)
{
    return s_frameskip;
}

unsigned gpsp_settings_frameskip_threshold(void)
{
    return s_threshold;
}

void gpsp_settings_set_frameskip(int frameskip, unsigned threshold)
{
    if (frameskip < GPSP_SETTINGS_FRAMESKIP_AUTO ||
            frameskip > GPSP_SETTINGS_FRAMESKIP_MAX ||
            threshold < GPSP_SETTINGS_THRESHOLD_MIN ||
            threshold > GPSP_SETTINGS_THRESHOLD_MAX)
        return;
    if (frameskip != s_frameskip || threshold != s_threshold)
        s_core_update = true;
    s_frameskip = frameskip;
    s_threshold = threshold;
    gpsp_board_video_frameskip(frameskip);
}

bool gpsp_settings_take_core_update(void)
{
    bool update = s_core_update;

    s_core_update = false;
    return update;
}

bool gpsp_settings_auto_save(void)
{
    return s_auto_save;
}

bool gpsp_settings_auto_load(void)
{
    return s_auto_load;
}

void gpsp_settings_set_auto(bool save, bool load)
{
    s_auto_save = save;
    s_auto_load = load;
}

unsigned gpsp_settings_control(enum gpsp_settings_control control)
{
    return s_controls[control];
}

bool gpsp_settings_set_control(enum gpsp_settings_control control,
        unsigned button)
{
    if (button == GPSP_SETTINGS_BUTTON_NONE)
    {
        s_controls[control] = button;
        return true;
    }
    if (button >= GPSP_SETTINGS_BUTTONS || !s_button_names[button])
        return false;
    for (int i = 0; i < GPSP_SETTINGS_CONTROL_COUNT; i++)
        if (s_controls[i] == button)
            s_controls[i] = s_controls[control];
    s_controls[control] = button;
    return true;
}

void gpsp_settings_reset_controls(void)
{
    memcpy(s_controls, s_control_defaults, sizeof(s_controls));
}

const char *gpsp_settings_button_name(unsigned button)
{
    if (button == GPSP_SETTINGS_BUTTON_NONE)
        return "None";
    if (button >= GPSP_SETTINGS_BUTTONS || !s_button_names[button])
        return "?";
    return s_button_names[button];
}

unsigned gpsp_settings_fast_forward_speed(void)
{
    return s_fast_forward_speed;
}

bool gpsp_settings_fast_forward_toggle(void)
{
    return s_fast_forward_toggle;
}

void gpsp_settings_set_fast_forward(unsigned speed, bool toggle)
{
    if (speed < GPSP_SETTINGS_FAST_FORWARD_MIN ||
            speed > GPSP_SETTINGS_FAST_FORWARD_MAX)
        return;
    s_fast_forward_speed = speed;
    s_fast_forward_toggle = toggle;
}

bool gpsp_settings_last_rom(char *name, size_t size)
{
    return gpsp_storage_config("last_rom", name, size);
}

void gpsp_settings_note_rom(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    char last[256];
    struct gpsp_storage_setting setting = { "last_rom", name };

    if (!gpsp_settings_last_rom(last, sizeof(last)) || strcmp(last, name))
        gpsp_storage_config_set(&setting, 1);
}

void gpsp_settings_wifi(struct gpsp_settings_wifi *wifi)
{
    if (!gpsp_storage_config("wifi_ssid", wifi->ssid, sizeof(wifi->ssid)))
        wifi->ssid[0] = '\0';
    if (!gpsp_storage_config("wifi_password", wifi->password,
            sizeof(wifi->password)))
        wifi->password[0] = '\0';
    if (!gpsp_storage_config("wifi_psk", wifi->psk, sizeof(wifi->psk)) ||
            strlen(wifi->psk) != 64)
        wifi->psk[0] = '\0';
}

bool gpsp_settings_set_wifi(const struct gpsp_settings_wifi *wifi)
{
    const struct gpsp_storage_setting settings[] = {
        { "wifi_ssid", wifi->ssid },
        { "wifi_password", wifi->password },
        { "wifi_psk", wifi->psk },
    };
    bool saved = gpsp_storage_config_set(settings, 3);

    if (saved)
        ESP_LOGI(TAG, "Wi-Fi saved: %s", !wifi->ssid[0] ? "none" :
                wifi->psk[0] ? "WPA2 key" : wifi->password[0] ? "password" :
                "open network");
    return saved;
}
