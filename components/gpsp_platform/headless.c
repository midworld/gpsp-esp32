/* Headless transport for physical boards without host output.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_transport.h"

#include "esp_log.h"

esp_err_t gpsp_espemu_transport_init(void)
{
    ESP_LOGI("gpsp_board", "Headless mode: display, audio and network disabled");
    return ESP_OK;
}

#if CONFIG_GPSP_BENCHMARK
bool gpsp_espemu_transport_connected(void)
{
    return true;
}

void gpsp_espemu_transport_get_output_stats(
        uint32_t *submitted, uint32_t *sent, uint32_t *dropped)
{
    *submitted = *sent = *dropped = 0;
}
#endif

bool gpsp_espemu_transport_submit_frame(
        const uint16_t *pixels, unsigned width, unsigned height, size_t pitch)
{
    (void)pixels;
    (void)width;
    (void)height;
    (void)pitch;
    return false;
}

uint32_t gpsp_espemu_transport_session(void)
{
    return 0;
}

uint16_t gpsp_espemu_transport_read_input(void)
{
    return 0;
}

struct gpsp_board_request gpsp_espemu_transport_read_command(void)
{
    return (struct gpsp_board_request){0};
}

bool gpsp_espemu_transport_command_valid(struct gpsp_board_request request)
{
    return !request.session;
}

void gpsp_espemu_transport_report_command(
        struct gpsp_board_request request, bool success)
{
    (void)request;
    (void)success;
}

void gpsp_espemu_transport_shutdown(void)
{
}
