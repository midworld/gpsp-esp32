/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_TOUCH_H
#define GPSP_TOUCH_H

#include "esp_err.h"
#include "gpsp_virtual_pad.h"

esp_err_t gpsp_touch_init(void (*changed)(void));
void gpsp_touch_shutdown(void);
bool gpsp_touch_available(void);
bool gpsp_touch_enabled(void);
void gpsp_touch_set_enabled(bool enabled);
void gpsp_touch_cancel(void);
/* One atomic snapshot: visibility in bit 31, logical buttons in bits 0..16. */
uint32_t gpsp_touch_read(void);

#endif
