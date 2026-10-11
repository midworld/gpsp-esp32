/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>

#include "iperf_api.h"
#include "iperf3_tcp.h"

#define IPERF_TCP_BLOCK_SIZE (16 * 1024)

int iperf3_tcp_run(const char *host, uint16_t port, unsigned seconds,
        bool reverse)
{
    struct iperf_test *test = iperf_new_test();
    if (test == NULL)
    {
        printf("iperf3: cannot create test\n");
        return 0;
    }
    if (iperf_defaults(test) != 0)
    {
        printf("iperf3: cannot initialize test: %s\n", iperf_strerror(i_errno));
        iperf_free_test(test);
        return 0;
    }

    iperf_set_test_role(test, 'c');
    iperf_set_test_server_hostname(test, host);
    iperf_set_test_server_port(test, port);
    iperf_set_test_duration(test, seconds);
    iperf_set_test_reverse(test, reverse);
    iperf_set_test_blksize(test, IPERF_TCP_BLOCK_SIZE);
    iperf_set_test_repeating_payload(test, true);
    iperf_set_test_reporter_interval(test, 1);
    iperf_set_test_stats_interval(test, 1);

    printf("iperf3 TCP %s (%s:%u, %u s)\n", reverse ? "RX" : "TX",
            host, port, seconds);
    int result = iperf_run_client(test);
    if (result != 0)
        printf("iperf3: %s\n", iperf_strerror(i_errno));
    iperf_free_test(test);
    return result == 0;
}
