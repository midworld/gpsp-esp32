/* Serial port wrapper for the host viewer's serial input mode.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_ESPEMU_HOST_SERIAL_H
#define GPSP_ESPEMU_HOST_SERIAL_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Open a serial port at 115200 baud, 8N1, without blocking reads. DTR and
 * RTS stay released so that opening does not reset the board.
 *
 * @param path Device path such as /dev/cu.usbmodemXXXX.
 * @return A port handle, or -1 on failure.
 */
int gpsp_host_serial_open(const char *path);

/** Reset the board by pulsing RTS, which drives EN on ESP32 boards. */
void gpsp_host_serial_reset(int port);

/** @return The byte count, zero when no data is ready, or -1 on error. */
int gpsp_host_serial_read(int port, void *data, size_t size);

/** @return true after all bytes are written. */
bool gpsp_host_serial_write(int port, const void *data, size_t size);

void gpsp_host_serial_close(int port);

#endif /* GPSP_ESPEMU_HOST_SERIAL_H */
