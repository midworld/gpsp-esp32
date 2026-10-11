/* Raw .sav files on the SD card.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_SAVE_FILE_H
#define GPSP_SAVE_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Save bytes in a file of the given length: a standard save size, allowing
 * a short trailer such as RTC data. With a nonzero expected size, only that
 * size counts (512 B and 8 KiB EEPROM count as one). Zero otherwise.
 */
size_t gpsp_save_file_size(long length, size_t expected);

/**
 * Read a raw save of the expected size (zero: any) into data.
 *
 * @return The save bytes read, or zero for a missing or invalid file.
 */
size_t gpsp_save_file_read(const char *path, uint8_t *data, size_t capacity,
        size_t expected);

/**
 * Replace path with a complete copy of data. A previous file valid for this
 * size stays as path.bak until the new one has its name.
 */
bool gpsp_save_file_write(const char *path, const uint8_t *data, size_t size);

#endif /* GPSP_SAVE_FILE_H */
