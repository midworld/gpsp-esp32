/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_DSI_OVERLAY_H
#define GPSP_DSI_OVERLAY_H

#include <stdint.h>
#include "esp_err.h"

/* ARGB8888 source dimensions and crop origin, in pixels. */
struct dsi_overlay_crop
{
    const uint32_t *pixels;
    unsigned pitch, rows, x, y;
};

esp_err_t gpsp_dsi_blend_overlay_crop(uint16_t *frame,
        const struct dsi_overlay_crop *crop, unsigned width, unsigned height,
        unsigned x, unsigned y, unsigned opacity);

/* Native panel coordinates; foreground rows are packed ARGB8888. */
esp_err_t gpsp_dsi_blend_overlay(uint16_t *frame, const uint32_t *foreground,
        unsigned width, unsigned height, unsigned x, unsigned y);
esp_err_t gpsp_dsi_blend_overlay_alpha(uint16_t *frame, const uint32_t *foreground,
        unsigned width, unsigned height, unsigned x, unsigned y, unsigned opacity);

#endif
