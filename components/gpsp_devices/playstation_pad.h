/* Shared USB and Bluetooth PlayStation report decoding.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_PLAYSTATION_PAD_H
#define GPSP_PLAYSTATION_PAD_H

#include <stdbool.h>
#include <stdint.h>

bool gpsp_playstation_decode(const uint8_t *report, unsigned size,
        bool dualsense, bool bluetooth, unsigned *buttons);
uint32_t gpsp_playstation_crc(const uint8_t *data, unsigned size, uint8_t prefix);

#endif
