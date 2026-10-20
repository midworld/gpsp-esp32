/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_DSI_HUD_H
#define GPSP_DSI_HUD_H

#include "sdkconfig.h"
#include <stdint.h>
#include "esp_err.h"
#include "lcd_status.h"

void gpsp_dsi_hud_init(void);
void gpsp_dsi_hud_shutdown(void);
void gpsp_dsi_hud_update(const struct lcd_status_value *values);
esp_err_t gpsp_dsi_hud_draw(uint16_t *frame);
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
void gpsp_dsi_hud_sample(bool enabled, unsigned ppa, unsigned expand,
        unsigned present, unsigned blend, unsigned copy);
#endif

#endif
