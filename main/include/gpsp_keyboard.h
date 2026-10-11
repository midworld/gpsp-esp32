/* Screen keyboard: type text with the pad.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_KEYBOARD_H
#define GPSP_KEYBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpsp_ui.h"

/**
 * Edit text on the full-screen UI. Printable ASCII only.
 *
 * @param pixels The UI picture from gpsp_board_ui_begin().
 * @param frame The paused game frame behind the keyboard, or NULL.
 * @param title What the text is for, shown above it.
 * @param text The text to edit; holds the result.
 * @param size The size of text, its terminating zero included.
 * @param secret Show the text as '*' until X shows it.
 * @return GPSP_UI_DONE on OK; otherwise text is unchanged: Select gives
 *         GPSP_UI_BACK, Home GPSP_UI_RESUME, and a host request to close
 *         the game GPSP_UI_QUIT.
 */
enum gpsp_ui_exit gpsp_keyboard_run(uint16_t *pixels, const uint16_t *frame,
        const char *title, char *text, size_t size, bool secret);

#endif /* GPSP_KEYBOARD_H */
