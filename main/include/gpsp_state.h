/* Manual full-state files: the core's serialized state in slots per ROM.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_STATE_H
#define GPSP_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Slots 0 to 9, plus the automatic one. */
#define GPSP_STATE_SLOTS 10
#define GPSP_STATE_AUTO (-1)

/* Pictures are game frames. */
#define GPSP_STATE_PICTURE_WIDTH 240
#define GPSP_STATE_PICTURE_HEIGHT 160

bool gpsp_state_start(void);

/* The slot that serial and host commands use. */
int gpsp_state_slot(void);
void gpsp_state_select(int slot);

/**
 * Save a slot. frame (a 240x160 RGB565 game frame) is kept next to it as a
 * PNG picture; NULL removes the old picture.
 */
bool gpsp_state_save(int slot, const uint16_t *frame, size_t pitch);
bool gpsp_state_load(int slot);
bool gpsp_state_used(int slot);

/** Read a slot's picture into 240x160 RGB565 pixels. */
bool gpsp_state_picture(int slot, uint16_t *pixels);

/** Read a state picture file, such as another ROM's, the same way. */
bool gpsp_state_read_picture(const char *path, uint16_t *pixels);

#endif /* GPSP_STATE_H */
