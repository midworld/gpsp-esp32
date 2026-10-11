/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* Adapted from ESP-Hosted examples/host_hosted_events/main/station_example.c. */

#include "esp_netif.h"
#include "esp_wifi.h"

#include "sample_wifi.h"

esp_err_t sample_wifi_start(wifi_config_t *wifi_config)
{
  if (wifi_config != NULL && esp_netif_create_default_wifi_sta() == NULL)
    return ESP_FAIL;

  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_wifi_init(&config);
  if (err == ESP_OK)
    err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err == ESP_OK && wifi_config != NULL)
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (err == ESP_OK && wifi_config != NULL)
    err = esp_wifi_set_config(WIFI_IF_STA, wifi_config);
  if (err == ESP_OK)
    err = esp_wifi_start();
  if (err == ESP_OK && wifi_config != NULL)
    err = esp_wifi_set_ps(WIFI_PS_NONE);
  return err;
}
