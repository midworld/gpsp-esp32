/* Raw .sav files on the SD card.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_save_file.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define GPSP_SAVE_FILE_TRAILER 64

/* EEPROM size is known only after the game uses it, so 512 B and 8 KiB
   stay interchangeable. */
static bool gpsp_save_file_fits(size_t size, size_t expected)
{
    if (!expected)
        return true;
    if (expected == 512 || expected == 8 * 1024)
        return size == 512 || size == 8 * 1024;
    return size == expected;
}

size_t gpsp_save_file_size(long length, size_t expected)
{
    static const size_t sizes[] = {
        128 * 1024, 64 * 1024, 32 * 1024, 8 * 1024, 512,
    };

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        if (gpsp_save_file_fits(sizes[i], expected) &&
                length >= (long)sizes[i] &&
                length - (long)sizes[i] <= GPSP_SAVE_FILE_TRAILER)
            return sizes[i];
    return 0;
}

static size_t gpsp_save_file_length(FILE *file, size_t expected)
{
    long length;

    if (fseek(file, 0, SEEK_END) != 0)
        return 0;
    length = ftell(file);
    if (fseek(file, 0, SEEK_SET) != 0)
        return 0;
    return gpsp_save_file_size(length, expected);
}

size_t gpsp_save_file_read(const char *path, uint8_t *data, size_t capacity,
        size_t expected)
{
    FILE *file = fopen(path, "rb");
    size_t size;
    bool valid;

    if (!file)
        return 0;

    size = gpsp_save_file_length(file, expected);
    valid = size && size <= capacity && fread(data, size, 1, file) == 1;
    fclose(file);
    return valid ? size : 0;
}

static bool gpsp_save_file_valid(const char *path, size_t expected)
{
    FILE *file = fopen(path, "rb");
    size_t size;

    if (!file)
        return false;
    size = gpsp_save_file_length(file, expected);
    fclose(file);
    return size != 0;
}

bool gpsp_save_file_write(const char *path, const uint8_t *data, size_t size)
{
    char temp[320];
    char backup[320];
    FILE *file;
    bool valid;

    snprintf(temp, sizeof(temp), "%s.tmp", path);
    snprintf(backup, sizeof(backup), "%s.bak", path);

    file = fopen(temp, "wb");
    if (!file)
        return false;

    valid = fwrite(data, size, 1, file) == 1 && fflush(file) == 0 &&
            fsync(fileno(file)) == 0;
    if (fclose(file) != 0)
        valid = false;
    if (!valid)
    {
        remove(temp);
        return false;
    }

    /* Rotate only a valid file; otherwise keep the .bak a load fell back to. */
    if (gpsp_save_file_valid(path, size))
    {
        remove(backup);
        if (rename(path, backup) != 0)
        {
            remove(temp);
            return false;
        }
    }
    else
        remove(path);

    if (rename(temp, path) != 0)
        return false;
    remove(backup);
    return true;
}
