/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P4_NANO_IPERF3_TCP_H
#define P4_NANO_IPERF3_TCP_H

#include <stdbool.h>
#include <stdint.h>

int iperf3_tcp_run(const char *host, uint16_t port, unsigned seconds,
    bool reverse);

#endif
