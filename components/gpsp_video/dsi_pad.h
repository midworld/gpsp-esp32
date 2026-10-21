/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GPSP_DSI_PAD_H
#define GPSP_DSI_PAD_H
#include <stdint.h>
#include "esp_err.h"
/* Native panel rows [first, last); cached borders remain untouched. */
esp_err_t gpsp_dsi_pad_draw(uint16_t *frame, uint32_t state, unsigned opacity,
        unsigned first, unsigned last);
void gpsp_dsi_pad_shutdown(void);
void gpsp_dsi_pad_status(void);
#endif
