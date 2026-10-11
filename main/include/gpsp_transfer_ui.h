/* The File Transfer screen of the menu.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_TRANSFER_UI_H
#define GPSP_TRANSFER_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "gpsp_ui.h"

/**
 * Serve the SD card over Wi-Fi until B or Home. Wi-Fi stops on return.
 *
 * @param game_changed Set when files of the running game are uploaded or deleted.
 * @return GPSP_UI_BACK for B, GPSP_UI_RESUME for Home, GPSP_UI_QUIT for a
 *         host request to close the game.
 */
enum gpsp_ui_exit gpsp_transfer_ui_run(uint16_t *pixels,
        const uint16_t *frame, bool *game_changed);

#endif /* GPSP_TRANSFER_UI_H */
