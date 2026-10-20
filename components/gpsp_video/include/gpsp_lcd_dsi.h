/* DSI output. Drawing is serialized by the LCD draw lock.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_LCD_DSI_H
#define GPSP_LCD_DSI_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "gpsp_display.h"

esp_err_t gpsp_dsi_init(void);
esp_err_t gpsp_dsi_draw(const uint16_t *pixels, bool ui);
esp_err_t gpsp_dsi_ui_update(const uint32_t *overlay);
esp_err_t gpsp_dsi_ui_draw(const uint16_t *pixels, const uint16_t *background);
esp_err_t gpsp_dsi_clear(void);
void gpsp_dsi_shutdown(void);
void gpsp_dsi_options(enum gpsp_display_filter *filter,
        enum gpsp_display_color *color);
void gpsp_dsi_set_options(enum gpsp_display_filter filter,
        enum gpsp_display_color color);
enum gpsp_display_stats gpsp_dsi_stats_mode(void);
void gpsp_dsi_set_stats(enum gpsp_display_stats mode);
unsigned gpsp_dsi_pad_opacity(void);
void gpsp_dsi_set_pad_opacity(unsigned percent);
void gpsp_dsi_status(void);

#endif
