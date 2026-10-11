/* gpSP storage interface.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_STORAGE_H
#define GPSP_STORAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "sdkconfig.h"

/**
 * Mount storage, pick the ROM (SD card, then flash) and verify it exists.
 * Without a named card ROM, the menu lets Game List pick one or File
 * Transfer upload one.
 *
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
esp_err_t gpsp_storage_mount(void);

/**
 * Return the absolute path of the chosen GBA ROM.
 *
 * @return A persistent path string, or NULL while the game list has to
 *         pick one.
 */
const char *gpsp_storage_rom_path(void);

/* Resolve card path aliases to FAT names; a missing final file is kept. */
bool gpsp_storage_card_path(const char *path, char *out, size_t size);

typedef void (*gpsp_storage_rom_found_fn)(const char *path, void *user);

/**
 * Find the .gba files on the card: roms/ first, then the card root.
 * Names starting with '.' are skipped.
 *
 * @param found Called with each full path; may be NULL to only count.
 * @return The number of ROMs.
 */
size_t gpsp_storage_find_roms(gpsp_storage_rom_found_fn found, void *user);

/** The slot 0 state file of a card ROM; other state files add to it. */
bool gpsp_storage_state_path_for(const char *rom_path, char *path,
        size_t size);

#if CONFIG_GPSP_SD_CARD && CONFIG_GPSP_MENU
/** Use a card ROM picked from the game list at boot. */
esp_err_t gpsp_storage_set_rom(const char *path, bool skip_auto_load);

/** True while the SD card is mounted. */
bool gpsp_storage_card(void);

/** Use the ROM asked for by gpsp_storage_request_rom(). */
esp_err_t gpsp_storage_use_requested_rom(void);

/** Start a card ROM after the running one, or after a software restart. */
void gpsp_storage_request_rom(const char *path, bool skip_auto_load);
bool gpsp_storage_rom_requested(void);

/** True when the chosen ROM starts without its automatic state. */
bool gpsp_storage_skip_auto_load(void);
#endif

/**
 * Return the raw .sav path on the SD card for a card ROM, or NULL to use
 * the flash save partition.
 */
const char *gpsp_storage_save_path(void);

/** Return the full-state file path for the current ROM. */
const char *gpsp_storage_state_path(void);

/**
 * Read "key=value" from gpsp.cfg on the SD card.
 *
 * @return true when the key has a non-empty value.
 */
bool gpsp_storage_config(const char *key, char *value, size_t size);

struct gpsp_storage_setting
{
    const char *key;
    const char *value;
};

/**
 * Set keys in gpsp.cfg on the SD card. Other lines stay as they are; keys
 * not in the file are added at its end.
 *
 * @param count At most 32 settings.
 * @return false without a card or when writing failed.
 */
bool gpsp_storage_config_set(const struct gpsp_storage_setting *settings,
        size_t count);

void gpsp_storage_unmount(void);

#endif /* GPSP_STORAGE_H */
