/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modified for iperf3 connected streams, 2026.
 * Adapted from iperf_client_loop and iperf_server_loop in espressif/iperf v1.0.2.
 */

#include <errno.h>
#include <sys/socket.h>

#include "esp_iperf_io.h"
#include "net.h"

int esp_iperf_tcp_send(int socket, const void *buffer, size_t length)
{
    int actual_send = sendto(socket, buffer, length, 0, NULL, 0);
    if (actual_send >= 0)
        return actual_send;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
        errno == ENOBUFS)
        return 0;
    return NET_HARDERROR;
}

int esp_iperf_tcp_recv(int socket, void *buffer, size_t length)
{
    int actual_recv = recvfrom(socket, buffer, length, 0, NULL, NULL);
    if (actual_recv >= 0)
        return actual_recv;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        return 0;
    return NET_HARDERROR;
}
