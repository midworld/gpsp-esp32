/* Frame buffer hand-off between gpSP and a frontend.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_SCREEN_H
#define GPSP_SCREEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 16-bit pixels in the core's output format, 240 pixels per row, 161 rows:
 * the renderer may touch one row past the 160 visible ones. */
#define GPSP_SCREEN_BUFFER_BYTES (240u * 161u * 2u)

/**
 * Render the next frames into pixels (GPSP_SCREEN_BUFFER_BYTES or more).
 * Call only between frames, e.g. from the video callback, with interframe
 * blending disabled. Restore the core's buffer before unloading or deinit.
 * NULL returns the current buffer without replacing it.
 *
 * @return The buffer the core used until now.
 */
uint16_t *gpsp_screen_set_buffer(uint16_t *pixels);

#ifdef __cplusplus
}
#endif

#endif /* GPSP_SCREEN_H */
