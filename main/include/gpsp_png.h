/* PNG files to and from RGB565 pictures.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_PNG_H
#define GPSP_PNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Takes the next piece of the file; false stops the encoder. */
typedef bool (*gpsp_png_write_fn)(const void *data, size_t size, void *user);

/**
 * Encode an RGB565 picture as an 8-bit RGB PNG.
 *
 * @param pitch Bytes between two source rows.
 * @return false if memory ran out or write() failed.
 */
bool gpsp_png_encode(const uint16_t *pixels, unsigned width, unsigned height,
        size_t pitch, gpsp_png_write_fn write, void *user);

/**
 * Decode an 8-bit RGB or RGBA PNG of exactly width x height pixels into
 * RGB565 (pitch: width pixels). Interlaced files are not read.
 *
 * @return false for another format or size, or a damaged file.
 */
bool gpsp_png_decode(const uint8_t *data, size_t size, uint16_t *pixels,
        unsigned width, unsigned height);

#endif /* GPSP_PNG_H */
