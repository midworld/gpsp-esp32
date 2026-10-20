/* LCD output and UI ownership.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_LCD_H
#define GPSP_LCD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

esp_err_t gpsp_lcd_init(void);

/**
 * Queue a frame for the LCD task; copy it if it is not a claimed buffer.
 *
 * @return false if the frame was dropped.
 */
bool gpsp_lcd_present(const uint16_t *pixels, unsigned width,
        unsigned height, size_t pitch);

/**
 * Claim a free buffer for the core to render the next frame into. Pass it
 * back through gpsp_lcd_present() to show it without a copy.
 *
 * @return A GPSP_SCREEN_BUFFER_BYTES buffer, or NULL if none is free.
 */
uint16_t *gpsp_lcd_next_buffer(void);

/** Return an unsubmitted buffer; queued, sending and UI-held frames stay owned. */
void gpsp_lcd_release_buffer(uint16_t *pixels);

/** Set frameskip for FPS warning colors: 0 for off, below 0 for auto. */
void gpsp_lcd_frameskip(int frameskip);

/** Show CPU use and speed (%), temperature (tenths of C; INT_MIN: unknown),
 * free/minimum internal RAM (KiB) and PSRAM use.
 * Used PSRAM is in tenths of MiB; total is in MiB. */
void gpsp_lcd_status(unsigned cpu0, unsigned cpu1, int temp_tenths,
        unsigned speed, unsigned ram_kib, unsigned min_kib,
        unsigned psram_used_tenths, unsigned psram_mib);

#if CONFIG_GPSP_MENU
/**
 * Stop sending game frames and give a 320x240 picture for the UI. The
 * last game frame is kept for the UI until gpsp_lcd_ui_end().
 *
 * @return false without a panel.
 */
bool gpsp_lcd_ui_begin(uint16_t **pixels, const uint16_t **frame);
#if CONFIG_GPSP_LCD_DSI
uint32_t *gpsp_lcd_ui_layer(uint16_t *pixels, uint16_t **background);
#endif

/** Send rows y0 to y1 (exclusive) of the UI picture and wait for them. */
void gpsp_lcd_ui_draw(unsigned y0, unsigned y1);

/** Clear the panel and send game frames again. */
void gpsp_lcd_ui_end(void);
#endif

void gpsp_lcd_shutdown(void);
#if CONFIG_GPSP_LCD_DSI
/* Wake the display worker after a touch overlay change. */
void gpsp_lcd_overlay_changed(void);
#endif

#endif /* GPSP_LCD_H */
