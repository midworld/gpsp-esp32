/* Cross-platform socket wrapper for the ESP-EMU host viewer.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_ESPEMU_HOST_NET_H
#define GPSP_ESPEMU_HOST_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef intptr_t gpsp_host_socket_t;

#define GPSP_HOST_INVALID_SOCKET ((gpsp_host_socket_t)-1)

/** @return true after the process-wide socket API is ready. */
bool gpsp_host_net_init(void);

/** Release process-wide socket API resources. */
void gpsp_host_net_shutdown(void);

/**
 * Open a non-blocking IPv4 TCP listener.
 *
 * @param address Local address to bind.
 * @param port Local TCP port.
 * @return A socket handle, or GPSP_HOST_INVALID_SOCKET on failure.
 */
gpsp_host_socket_t gpsp_host_net_listen(
        const char *address, uint16_t port);

/**
 * Accept one non-blocking client connection.
 *
 * @param listener Listening socket returned by gpsp_host_net_listen().
 * @return A client handle, or GPSP_HOST_INVALID_SOCKET when none is ready.
 */
gpsp_host_socket_t gpsp_host_net_accept(gpsp_host_socket_t listener);

/**
 * Send bytes without blocking.
 *
 * @param socket_handle Connected client socket.
 * @param data Bytes to send.
 * @param size Maximum number of bytes to send.
 * @return The byte count, zero for a closed socket, or -1 on error.
 */
int gpsp_host_net_send(
        gpsp_host_socket_t socket_handle, const void *data, size_t size);

/**
 * Receive bytes without blocking.
 *
 * @param socket_handle Connected client socket.
 * @param data Destination for received bytes.
 * @param size Capacity of data in bytes.
 * @return The byte count, zero for a closed socket, or -1 on error.
 */
int gpsp_host_net_receive(
        gpsp_host_socket_t socket_handle, void *data, size_t size);

/** @return true if the last socket error means to retry later. */
bool gpsp_host_net_would_block(void);

/**
 * Close a listener or client socket.
 *
 * @param socket_handle Socket to close; an invalid handle is ignored.
 */
void gpsp_host_net_close(gpsp_host_socket_t socket_handle);

#endif /* GPSP_ESPEMU_HOST_NET_H */
