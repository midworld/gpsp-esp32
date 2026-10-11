/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#ifndef SAMPLE_WIFI_H
#define SAMPLE_WIFI_H

#include "esp_err.h"
#include "esp_wifi.h"

esp_err_t sample_wifi_start(wifi_config_t *wifi_config);

#endif
