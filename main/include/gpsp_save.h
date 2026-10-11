/* Cartridge save persistence.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_SAVE_H
#define GPSP_SAVE_H

#include "esp_err.h"

esp_err_t gpsp_save_start(const char *rom_path);
void gpsp_save_poll(void);
/* Starts writing a change now, without waiting for the poll interval. */
void gpsp_save_flush(void);
/* Wait for an already queued write before serving the card. */
void gpsp_save_wait(void);
void gpsp_save_stop(void);
/* Writes nothing more, not even when stopping: the save file on the card
   was replaced and must stay. */
void gpsp_save_disable(void);

#endif /* GPSP_SAVE_H */
