/* Screen pictures in the console log.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_SCREENSHOT_H
#define GPSP_SCREENSHOT_H

#include <stddef.h>
#include <stdint.h>

/**
 * Print an RGB565 picture as a base64 PNG between SCREENSHOT_BEGIN and
 * SCREENSHOT_END lines; each data line starts with "SS ".
 */
void gpsp_screenshot_print(const uint16_t *pixels, unsigned width,
        unsigned height, size_t pitch);

#endif /* GPSP_SCREENSHOT_H */
