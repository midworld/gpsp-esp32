/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_DSI_HUD_H
#define GPSP_DSI_HUD_H

#include "sdkconfig.h"
#include <stdint.h>
#if CONFIG_GPSP_LCD_DSI_DMA_EXPAND
#include "esp_async_color_convert.h"
#endif
#include "esp_err.h"
#include "lcd_status.h"
#include "gpsp_display.h"

void gpsp_dsi_hud_init(void);
void gpsp_dsi_hud_shutdown(void);
void gpsp_dsi_hud_update(const struct lcd_status_value *values);
#if CONFIG_GPSP_LCD_DSI_DMA_EXPAND
/* Borrowed only after all expansion transfers have completed. */
void gpsp_dsi_hud_scratch(async_color_convert_handle_t converter,
        uint16_t *scratch, size_t bytes);
#endif
esp_err_t gpsp_dsi_hud_draw(uint16_t *frame, enum gpsp_display_stats mode);
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
void gpsp_dsi_hud_sample(enum gpsp_display_stats mode, unsigned ppa, unsigned expand,
        unsigned present, unsigned blend, unsigned copy);
#endif

#endif
