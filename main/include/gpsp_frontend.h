/* Minimal libretro frontend for gpSP.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_FRONTEND_H
#define GPSP_FRONTEND_H

#include "esp_err.h"

/**
 * Load a GBA ROM and run the core until an error stops it.
 *
 * @param rom_path Absolute path to the GBA ROM.
 * @return An ESP-IDF error code if startup fails or the loop stops.
 */
esp_err_t gpsp_frontend_run(const char *rom_path);

#endif /* GPSP_FRONTEND_H */
