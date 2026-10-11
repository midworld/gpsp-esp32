/* Host tests for raw .sav files.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gpsp_save_file.h"

int test_rename(const char *from, const char *to);

/* rename() calls left before one fails; negative never fails. */
static int rename_fail_after = -1;

int test_rename(const char *from, const char *to)
{
    if (rename_fail_after == 0)
    {
        rename_fail_after = -1;
        return -1;
    }
    if (rename_fail_after > 0)
        rename_fail_after--;
    return rename(from, to);
}

static char dir[] = "/tmp/gpsp-save-file-XXXXXX";
static char sav[128], bak[128], tmp[128];
static uint8_t data[128 * 1024], read_back[128 * 1024];
static int failures;

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
    } while (0)

static void put(const char *path, size_t size, uint8_t fill)
{
    FILE *file = fopen(path, "wb");
    for (size_t i = 0; i < size; i++)
        fputc(fill, file);
    fclose(file);
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void reset(void)
{
    remove(sav);
    remove(bak);
    remove(tmp);
}

int main(void)
{
    if (!mkdtemp(dir))
        return 1;
    snprintf(sav, sizeof(sav), "%s/game.sav", dir);
    snprintf(bak, sizeof(bak), "%s/game.sav.bak", dir);
    snprintf(tmp, sizeof(tmp), "%s/game.sav.tmp", dir);

    /* Accepted lengths. */
    CHECK(gpsp_save_file_size(512, 0) == 512);
    CHECK(gpsp_save_file_size(8192 + 16, 0) == 8192);
    CHECK(gpsp_save_file_size(32768, 0) == 32768);
    CHECK(gpsp_save_file_size(131072, 0) == 131072);
    CHECK(gpsp_save_file_size(1, 0) == 0);
    CHECK(gpsp_save_file_size(1000, 0) == 0);
    CHECK(gpsp_save_file_size(131072 + 65, 0) == 0);

    /* A new save leaves only the .sav. */
    reset();
    memset(data, 0x11, sizeof(data));
    CHECK(gpsp_save_file_write(sav, data, 32768));
    CHECK(!exists(bak) && !exists(tmp));
    CHECK(gpsp_save_file_read(sav, read_back, sizeof(read_back), 0) == 32768);
    CHECK(read_back[0] == 0x11 && read_back[32767] == 0x11);

    /* A 1-byte .sav is rejected, so a load falls back to the .bak. */
    reset();
    put(sav, 1, 0x00);
    put(bak, 8192, 0x22);
    CHECK(gpsp_save_file_read(sav, read_back, sizeof(read_back), 0) == 0);
    CHECK(gpsp_save_file_read(bak, read_back, sizeof(read_back), 0) == 8192);

    /* The ROM's save type picks the size: a cut 128 KiB flash save is
       rejected, so its .bak is used. EEPROM sizes are interchangeable. */
    CHECK(gpsp_save_file_size(32768, 131072) == 0);
    CHECK(gpsp_save_file_size(131072, 131072) == 131072);
    CHECK(gpsp_save_file_size(8192, 512) == 8192);
    CHECK(gpsp_save_file_size(512, 8192) == 512);
    CHECK(gpsp_save_file_size(32768, 8192) == 0);
    reset();
    put(sav, 32768, 0x88);
    put(bak, 131072, 0x99);
    CHECK(gpsp_save_file_read(sav, read_back, sizeof(read_back), 131072) == 0);
    CHECK(gpsp_save_file_read(bak, read_back, sizeof(read_back), 131072) ==
            131072);

    /* A failed 128 KiB install does not rotate a cut .sav over the .bak. */
    rename_fail_after = 0;
    CHECK(!gpsp_save_file_write(sav, data, 131072));
    CHECK(gpsp_save_file_read(bak, read_back, sizeof(read_back), 131072) ==
            131072 && read_back[0] == 0x99);

    /* A trailer after the save is ignored. */
    reset();
    put(sav, 32768 + 16, 0x33);
    CHECK(gpsp_save_file_read(sav, read_back, sizeof(read_back), 0) == 32768);

    /* Only a .bak: a failed install keeps it. */
    reset();
    put(bak, 8192, 0x44);
    rename_fail_after = 0;
    CHECK(!gpsp_save_file_write(sav, data, 8192));
    CHECK(gpsp_save_file_read(bak, read_back, sizeof(read_back), 0) == 8192 &&
            read_back[0] == 0x44);

    /* A corrupt .sav and a valid .bak: a failed install keeps the .bak. */
    reset();
    put(sav, 1, 0x00);
    put(bak, 8192, 0x55);
    rename_fail_after = 0;
    CHECK(!gpsp_save_file_write(sav, data, 8192));
    CHECK(gpsp_save_file_read(bak, read_back, sizeof(read_back), 0) == 8192 &&
            read_back[0] == 0x55);

    /* A valid .sav: a failed install leaves the old save as .bak. */
    reset();
    put(sav, 8192, 0x66);
    rename_fail_after = 1;
    CHECK(!gpsp_save_file_write(sav, data, 8192));
    CHECK(!exists(sav));
    CHECK(gpsp_save_file_read(bak, read_back, sizeof(read_back), 0) == 8192 &&
            read_back[0] == 0x66);

    /* A valid .sav: a good write replaces it and drops the .bak. */
    reset();
    put(sav, 8192, 0x77);
    CHECK(gpsp_save_file_write(sav, data, 8192));
    CHECK(!exists(bak) && !exists(tmp));
    CHECK(gpsp_save_file_read(sav, read_back, sizeof(read_back), 0) == 8192 &&
            read_back[0] == 0x11);

    reset();
    rmdir(dir);
    if (failures)
        return 1;
    printf("save file ok\n");
    return 0;
}
