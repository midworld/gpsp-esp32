/* gpSP host transport interface.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_BOARDS_ESPEMU_TRANSPORT_H
#define GPSP_BOARDS_ESPEMU_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gpsp_board.h"
#include "sdkconfig.h"

/**
 * Start the selected network backend and host transport task.
 *
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
esp_err_t gpsp_espemu_transport_init(void);

#if CONFIG_GPSP_BENCHMARK
bool gpsp_espemu_transport_connected(void);
void gpsp_espemu_transport_get_output_stats(
        uint32_t *submitted, uint32_t *sent, uint32_t *dropped);
#endif

/**
 * Copy a frame into the non-blocking transport queue.
 *
 * @param pixels Pointer to the first RGB565 pixel.
 * @param width Frame width in pixels.
 * @param height Frame height in pixels.
 * @param pitch Number of bytes between two source rows.
 * @return true if queued, or false when no host is ready.
 */
bool gpsp_espemu_transport_submit_frame(
        const uint16_t *pixels, unsigned width, unsigned height, size_t pitch);

/** @return A nonzero ID of the current host connection, or zero when none. */
uint32_t gpsp_espemu_transport_session(void);

/** @return The latest libretro RetroPad bitmask received from the host. */
uint16_t gpsp_espemu_transport_read_input(void);
struct gpsp_board_request gpsp_espemu_transport_read_command(void);
bool gpsp_espemu_transport_command_valid(struct gpsp_board_request request);
void gpsp_espemu_transport_report_command(
        struct gpsp_board_request request, bool success);

/** Stop the transport and release its frame and network resources. */
void gpsp_espemu_transport_shutdown(void);

#endif /* GPSP_BOARDS_ESPEMU_TRANSPORT_H */
