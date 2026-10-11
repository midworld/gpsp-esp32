/* Manual full-state files hold only the core's serialized state.
 * Files are named after the ROM: <name>.state for slot 0, <name>.state1
 * to .state9 and <name>.state.auto. A PNG picture
 * of the game sits next to each, named <state file>.png.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_state.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "gpsp_png.h"
#include "gpsp_storage.h"
#include "libretro.h"

#define GPSP_STATE_EXT ".state"
/* Longest suffix after the slot 0 name: ".auto.png.tmp". */
#define GPSP_STATE_SUFFIX_MAX 13
/* Larger pictures are not ours. */
#define GPSP_STATE_PICTURE_MAX (1024 * 1024)

typedef bool (*gpsp_state_writer)(FILE *file, void *user);

struct gpsp_state_blob
{
    const uint8_t *data;
    size_t size;
};

struct gpsp_state_frame
{
    const uint16_t *pixels;
    size_t pitch;
};

static const char *TAG = "gpsp_state";
/* The slot 0 file; other names add to it. Empty without state storage. */
static char s_base[288];
static char s_path[sizeof(s_base)];
static char s_picture_path[sizeof(s_base)];
/* A name plus ".tmp" or ".bak". */
static char s_temp_path[sizeof(s_base) + 4];
static char s_backup_path[sizeof(s_base) + 4];
static int s_slot;

static bool gpsp_state_exists(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (file)
        fclose(file);
    return file != NULL;
}

/* A slot's file name plus a suffix. */
static bool gpsp_state_path(int slot, const char *suffix, char *path)
{
    int length;

    if (!s_base[0] || slot < GPSP_STATE_AUTO || slot >= GPSP_STATE_SLOTS)
        return false;
    if (slot == GPSP_STATE_AUTO)
        length = snprintf(path, sizeof(s_base), "%s.auto%s", s_base, suffix);
    else if (slot)
        length = snprintf(path, sizeof(s_base), "%s%d%s", s_base, slot,
                suffix);
    else
        length = snprintf(path, sizeof(s_base), "%s%s", s_base, suffix);
    return length > 0 && (size_t)length < sizeof(s_base);
}

/* Recovers only this ROM's slot files and pictures after a reset. */
static void gpsp_state_recover(void)
{
    const char *slash = strrchr(s_base, '/');
    size_t dir_length = slash ? (size_t)(slash - s_base) : 0;
    const char *base = slash ? slash + 1 : s_base;
    size_t base_length = strlen(base);
    struct dirent *entry;
    DIR *dir;

    if (!slash)
        return;
    snprintf(s_path, sizeof(s_path), "%.*s", (int)dir_length, s_base);
    dir = opendir(s_path);
    if (!dir)
        return;
    while ((entry = readdir(dir)))
    {
        const char *name = entry->d_name;
        const char *suffix;
        int written;

        if (strncmp(name, base, base_length))
            continue;
        suffix = name + base_length;
        if (!strncmp(suffix, ".auto", 5))
            suffix += 5;
        else if (*suffix >= '1' && *suffix <= '9')
            suffix++;
        if (!strncmp(suffix, ".png", 4))
            suffix += 4;
        if (strcmp(suffix, ".tmp") && strcmp(suffix, ".bak"))
            continue;
        written = snprintf(s_temp_path, sizeof(s_temp_path), "%.*s/%s",
                (int)dir_length, s_base, name);
        if (written <= 0 || (size_t)written >= sizeof(s_temp_path))
            continue;
        if (!strcmp(suffix, ".tmp"))
        {
            remove(s_temp_path);
            continue;
        }
        snprintf(s_path, sizeof(s_path), "%.*s", written - 4,
                s_temp_path);
        if (!gpsp_state_exists(s_path))
            rename(s_temp_path, s_path);
    }
    closedir(dir);
}

bool gpsp_state_start(void)
{
    const char *path = gpsp_storage_state_path();
    size_t length = path ? strlen(path) : 0;
    size_t stem = length - (sizeof(GPSP_STATE_EXT) - 1);

    s_base[0] = '\0';
    if (length < sizeof(GPSP_STATE_EXT) ||
            length + GPSP_STATE_SUFFIX_MAX >= sizeof(s_base) ||
            strcmp(path + stem, GPSP_STATE_EXT) != 0)
        return false;
    memcpy(s_base, path, length + 1);
    gpsp_state_recover();
    return true;
}

int gpsp_state_slot(void)
{
    return s_slot;
}

void gpsp_state_select(int slot)
{
    if (slot >= GPSP_STATE_AUTO && slot < GPSP_STATE_SLOTS)
        s_slot = slot;
}

/* Writes a file aside first so a failed write keeps the previous one. */
static bool gpsp_state_replace(const char *path, gpsp_state_writer write,
        void *user)
{
    FILE *file;
    bool valid;

    snprintf(s_temp_path, sizeof(s_temp_path), "%s.tmp", path);
    snprintf(s_backup_path, sizeof(s_backup_path), "%s.bak", path);
    file = fopen(s_temp_path, "wb");
    valid = file && write(file, user);
    if (file && fclose(file) != 0)
        valid = false;
    /* FAT cannot rename over a file: keep the old one as a backup until
       the new one is in place. Without the file the backup may be the only
       copy left by an earlier failure, so it stays. */
    if (valid)
    {
        bool had_file = gpsp_state_exists(path);

        if (had_file)
        {
            remove(s_backup_path);
            valid = rename(path, s_backup_path) == 0;
        }
        if (valid && rename(s_temp_path, path) != 0)
        {
            valid = false;
            if (had_file)
                rename(s_backup_path, path);
        }
        if (valid)
            remove(s_backup_path);
    }
    if (!valid)
        remove(s_temp_path);
    return valid;
}

static bool gpsp_state_write_blob(FILE *file, void *user)
{
    const struct gpsp_state_blob *blob = user;

    return fwrite(blob->data, blob->size, 1, file) == 1;
}

static bool gpsp_state_png_out(const void *data, size_t size, void *user)
{
    return fwrite(data, size, 1, user) == 1;
}

static bool gpsp_state_write_png(FILE *file, void *user)
{
    const struct gpsp_state_frame *frame = user;

    return gpsp_png_encode(frame->pixels, GPSP_STATE_PICTURE_WIDTH,
            GPSP_STATE_PICTURE_HEIGHT, frame->pitch, gpsp_state_png_out, file);
}

static uint8_t *gpsp_state_allocate(size_t *size)
{
    *size = retro_serialize_size();
    if (!s_base[0] || !*size)
        return NULL;
    return heap_caps_malloc(*size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

bool gpsp_state_save(int slot, const uint16_t *frame, size_t pitch)
{
    size_t size;
    uint8_t *data;
    bool valid;

    if (!gpsp_state_path(slot, "", s_path) ||
            !gpsp_state_path(slot, ".png", s_picture_path))
        return false;
    data = gpsp_state_allocate(&size);
    if (!data)
        return false;

    valid = retro_serialize(data, size);
    if (valid)
    {
        struct gpsp_state_blob blob = { data, size };

        valid = gpsp_state_replace(s_path, gpsp_state_write_blob, &blob);
    }
    heap_caps_free(data);

    if (valid)
    {
        struct gpsp_state_frame picture = { frame, pitch };

        /* An old picture would show another moment than the state. */
        if (!frame || !gpsp_state_replace(s_picture_path,
                gpsp_state_write_png, &picture))
            remove(s_picture_path);
        ESP_LOGI(TAG, "State saved: %s", s_path);
    }
    else
        ESP_LOGE(TAG, "State save failed: %s", s_path);
    return valid;
}

bool gpsp_state_load(int slot)
{
    size_t size;
    uint8_t *data;
    FILE *file;
    bool valid;

    if (!gpsp_state_path(slot, "", s_path))
        return false;
    data = gpsp_state_allocate(&size);
    if (!data)
        return false;

    /* The backup is the state while a failed replacement left no file. */
    snprintf(s_backup_path, sizeof(s_backup_path), "%s.bak", s_path);
    const char *path = gpsp_state_exists(s_path) ? s_path : s_backup_path;
    file = fopen(path, "rb");
    valid = file && fread(data, size, 1, file) == 1 &&
            fgetc(file) == EOF && !ferror(file);
    if (file)
        fclose(file);
    if (valid)
    {
        /* A state that fails partway through has already overwritten part
           of the game: snapshot first and put it back on failure. */
        uint8_t *current = heap_caps_malloc(size,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (!current || !retro_serialize(current, size))
            valid = false;
        else if (!retro_unserialize(data, size))
        {
            valid = false;
            if (!retro_unserialize(current, size))
                ESP_LOGE(TAG, "State restore after a failed load failed");
        }
        heap_caps_free(current);
    }

    heap_caps_free(data);
    if (valid)
        ESP_LOGI(TAG, "State loaded: %s", path);
    else
        ESP_LOGE(TAG, "State load failed or no state exists: %s", s_path);
    return valid;
}

bool gpsp_state_used(int slot)
{
    if (!gpsp_state_path(slot, "", s_path))
        return false;
    snprintf(s_backup_path, sizeof(s_backup_path), "%s.bak", s_path);
    return gpsp_state_exists(s_path) || gpsp_state_exists(s_backup_path);
}

bool gpsp_state_read_picture(const char *path, uint16_t *pixels)
{
    struct stat info;
    uint8_t *data;
    FILE *file;
    bool valid;

    if (stat(path, &info) != 0 || info.st_size <= 0 ||
            info.st_size > GPSP_STATE_PICTURE_MAX)
        return false;
    data = heap_caps_malloc(info.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data)
        return false;
    file = fopen(path, "rb");
    valid = file && fread(data, info.st_size, 1, file) == 1;
    if (file)
        fclose(file);
    valid = valid && gpsp_png_decode(data, info.st_size, pixels,
            GPSP_STATE_PICTURE_WIDTH, GPSP_STATE_PICTURE_HEIGHT);
    heap_caps_free(data);
    return valid;
}

bool gpsp_state_picture(int slot, uint16_t *pixels)
{
    return gpsp_state_path(slot, ".png", s_picture_path) &&
            gpsp_state_read_picture(s_picture_path, pixels);
}
