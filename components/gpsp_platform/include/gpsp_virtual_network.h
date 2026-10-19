/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_VIRTUAL_NETWORK_H
#define GPSP_VIRTUAL_NETWORK_H
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
esp_err_t gpsp_virtual_network_init(EventGroupHandle_t events, EventBits_t bit);
void gpsp_virtual_network_shutdown(void);
#endif
