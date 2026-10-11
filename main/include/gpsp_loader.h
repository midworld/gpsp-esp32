/* The game list: pick a card ROM on the full-screen UI.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_LOADER_H
#define GPSP_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum gpsp_loader_result
{
    GPSP_LOADER_CANCEL,
    GPSP_LOADER_PLAY,
    GPSP_LOADER_QUIT,
    /* Home in a list opened from the menu: back to the game. */
    GPSP_LOADER_RESUME,
};

/**
 * Show the card's ROMs and wait for a choice.
 *
 * @param pixels The UI picture from gpsp_board_ui_begin().
 * @param frame The paused game frame behind the list, or NULL.
 * @param path Set to the chosen ROM.
 * @param skip_auto_load Set when Select+A starts it without its auto
 *        state.
 * @return Play, cancel (B), resume (Home), or a host request to close the
 *         game.
 */
enum gpsp_loader_result gpsp_loader_run(uint16_t *pixels,
        const uint16_t *frame, char *path, size_t size, bool *skip_auto_load);

#endif /* GPSP_LOADER_H */
