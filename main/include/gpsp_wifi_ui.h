/* Wi-Fi network list for the menu.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_WIFI_UI_H
#define GPSP_WIFI_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpsp_ui.h"

/**
 * Scan for networks and let the pad pick one, or type a hidden one's
 * name. Wi-Fi must be open.
 *
 * @param ssid Set to the chosen network's name.
 * @return GPSP_UI_DONE with a network; GPSP_UI_BACK for B, GPSP_UI_RESUME
 *         for Home and GPSP_UI_QUIT for a host request to close the game.
 */
enum gpsp_ui_exit gpsp_wifi_ui_choose(uint16_t *pixels,
        const uint16_t *frame, char *ssid, size_t size);

#endif /* GPSP_WIFI_UI_H */
