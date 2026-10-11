/* HTTP file transfer for the SD card.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_TRANSFER_H
#define GPSP_TRANSFER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

struct gpsp_transfer_status
{
    /* The file in transfer, or empty. */
    char file[64];
    bool upload;
    uint32_t done;
    uint32_t total;
    unsigned uploads;
    unsigned downloads;
    /* A file of the running game changed. */
    bool game_changed;
};

/** Start the server on port 80; the network must be up. */
esp_err_t gpsp_transfer_start(void);

/** Recover an interrupted upload after the SD card is mounted. */
esp_err_t gpsp_transfer_recover(void);

/** Stop the server; a transfer in progress is cut off. */
void gpsp_transfer_stop(void);

void gpsp_transfer_status(struct gpsp_transfer_status *status);

#endif /* GPSP_TRANSFER_H */
