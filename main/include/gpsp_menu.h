/* Boot and in-game menu.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_MENU_H
#define GPSP_MENU_H

#include <stdbool.h>

enum gpsp_menu_result
{
    GPSP_MENU_RESUME,
    /* Close the game. */
    GPSP_MENU_QUIT,
    /* Close without rewriting files changed by File Transfer. */
    GPSP_MENU_QUIT_KEEP_FILES,
    /* Close the game and restart into the ROM picked from the game list. */
    GPSP_MENU_SWITCH,
    /* Restart from uploaded files without writing cartridge or auto saves. */
    GPSP_MENU_RELOAD,
};

/**
 * Show the menu and return when it closes.
 *
 * @param in_game False at boot: hide game actions and keep the menu open
 *        until a ROM is chosen or the host quits.
 */
enum gpsp_menu_result gpsp_menu_run(bool in_game);

#endif /* GPSP_MENU_H */
