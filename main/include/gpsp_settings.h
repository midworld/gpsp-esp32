/* Settings the menu changes, kept in gpsp.cfg.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_SETTINGS_H
#define GPSP_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include "gpsp_display.h"

/* Frameskip: 0 draws every frame, 1-9 frames are skipped after each drawn
   one, and auto skips while the speaker queue runs low. */
#define GPSP_SETTINGS_FRAMESKIP_AUTO (-1)
#define GPSP_SETTINGS_FRAMESKIP_MAX 9
#define GPSP_SETTINGS_THRESHOLD_MIN 10
#define GPSP_SETTINGS_THRESHOLD_MAX 200

/** Read the settings from gpsp.cfg and apply them. */
void gpsp_settings_load(void);

/** Write the settings that changed since they were read or written. */
void gpsp_settings_save(void);

int gpsp_settings_frameskip(void);
/* Auto frameskip starts below this speaker queue level (% of target). */
unsigned gpsp_settings_frameskip_threshold(void);
void gpsp_settings_set_frameskip(int frameskip, unsigned threshold);

/** True once after the frameskip changed, so the core reads it again. */
bool gpsp_settings_take_core_update(void);

/* Save the auto state slot when the game closes; load it when it starts. */
bool gpsp_settings_auto_save(void);
bool gpsp_settings_auto_load(void);
void gpsp_settings_set_auto(bool save, bool load);

bool gpsp_settings_display_available(void);
enum gpsp_display_filter gpsp_settings_display_filter(void);
enum gpsp_display_color gpsp_settings_display_color(void);
void gpsp_settings_set_display(enum gpsp_display_filter filter,
        enum gpsp_display_color color);

/* Functions a pad button can be given. Each has one button or none, and
   no two share one. The D-pad, Home and the menu's buttons stay fixed. */
enum gpsp_settings_control
{
    GPSP_SETTINGS_CONTROL_A,
    GPSP_SETTINGS_CONTROL_B,
    GPSP_SETTINGS_CONTROL_L,
    GPSP_SETTINGS_CONTROL_R,
    GPSP_SETTINGS_CONTROL_START,
    GPSP_SETTINGS_CONTROL_SELECT,
    GPSP_SETTINGS_CONTROL_TURBO_A,
    GPSP_SETTINGS_CONTROL_TURBO_B,
    GPSP_SETTINGS_CONTROL_FAST_FORWARD,
    GPSP_SETTINGS_CONTROL_COUNT,
};

/* A control without a button. */
#define GPSP_SETTINGS_BUTTON_NONE (~0u)

/** The RetroPad button ID given to a control, or
    GPSP_SETTINGS_BUTTON_NONE. */
unsigned gpsp_settings_control(enum gpsp_settings_control control);

/**
 * Give a RetroPad button, or GPSP_SETTINGS_BUTTON_NONE, to a control. A
 * control that had the button takes this control's old one.
 *
 * @return False for a button that cannot be given.
 */
bool gpsp_settings_set_control(enum gpsp_settings_control control,
        unsigned button);

void gpsp_settings_reset_controls(void);

/** A short name for a RetroPad button ID, such as "R2", or "None". */
const char *gpsp_settings_button_name(unsigned button);

#define GPSP_SETTINGS_FAST_FORWARD_MIN 2
#define GPSP_SETTINGS_FAST_FORWARD_MAX 3

/* Fast forward runs this many game frames for each one shown. */
unsigned gpsp_settings_fast_forward_speed(void);
/* False: fast forward runs while its button is held. True: each press
   turns it on or off. */
bool gpsp_settings_fast_forward_toggle(void);
void gpsp_settings_set_fast_forward(unsigned speed, bool toggle);

/** The file name of the ROM last started, from gpsp.cfg. */
bool gpsp_settings_last_rom(char *name, size_t size);

/** Write the ROM's file name to gpsp.cfg as the last one started. */
void gpsp_settings_note_rom(const char *path);

/* The Wi-Fi network the menu joins. A typed password comes first; after
   it joins a WPA2 network only its key stays. Spaces at either end of a
   value are lost, as gpsp.cfg trims them. Empty strings when not set. */
struct gpsp_settings_wifi
{
    char ssid[33];
    char password[65];
    /* The WPA2 key, 64 hex digits. */
    char psk[65];
};

void gpsp_settings_wifi(struct gpsp_settings_wifi *wifi);
bool gpsp_settings_set_wifi(const struct gpsp_settings_wifi *wifi);

#endif /* GPSP_SETTINGS_H */
