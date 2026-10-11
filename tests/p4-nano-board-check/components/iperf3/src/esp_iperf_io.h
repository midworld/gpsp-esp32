/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modified for iperf3 connected streams, 2026.
 */
#ifndef ESP_IPERF_IO_H
#define ESP_IPERF_IO_H

#include <stddef.h>

int esp_iperf_tcp_send(int socket, const void *buffer, size_t length);
int esp_iperf_tcp_recv(int socket, void *buffer, size_t length);

#endif
