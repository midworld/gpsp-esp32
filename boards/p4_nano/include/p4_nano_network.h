/* ESP32-P4-NANO-WIFI6-DB network interface.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_P4_NANO_NETWORK_H
#define GPSP_P4_NANO_NETWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

esp_err_t gpsp_p4_nano_network_init(
        EventGroupHandle_t events, EventBits_t ready_bit);
uint32_t gpsp_p4_nano_network_generation(void);
void gpsp_p4_nano_network_shutdown(void);

/* Wi-Fi for the menu: started only on request, never at boot. */
struct gpsp_p4_nano_wifi_ap
{
    char ssid[33];
    int8_t rssi;
    bool secure;
};

/** Start the Wi-Fi co-processor and the station, without connecting. */
esp_err_t gpsp_p4_nano_wifi_open(void);

/** Scan for access points, strongest first. */
size_t gpsp_p4_nano_wifi_scan(struct gpsp_p4_nano_wifi_ap *aps, size_t max);

/**
 * Join a network and wait for an address.
 *
 * @param password A passphrase, or a WPA2 key as 64 hex digits.
 * @param address Set to the IPv4 address on success.
 * @param psk Set to the WPA2 key of a passphrase that joined a WPA2
 *        network, as 64 hex digits; empty otherwise. At least 65 bytes.
 * @return ESP_ERR_NOT_FOUND without the network, ESP_ERR_INVALID_ARG when
 *         the password is refused, ESP_ERR_TIMEOUT without an answer.
 */
esp_err_t gpsp_p4_nano_wifi_connect(const char *ssid, const char *password,
        char *address, size_t size, char *psk, size_t psk_size);

/** End a join in progress from another task; it returns
    ESP_ERR_INVALID_STATE. */
void gpsp_p4_nano_wifi_cancel(void);

/**
 * Turn the open radio into a WPA2 access point.
 *
 * @param address Set to the access point's IPv4 address.
 */
esp_err_t gpsp_p4_nano_wifi_start_ap(const char *ssid, const char *password,
        char *address, size_t size);

/** Stop the radio; the next open starts it again quickly. */
void gpsp_p4_nano_wifi_close(void);

/** Release menu Wi-Fi resources after its workers have stopped. */
void gpsp_p4_nano_wifi_shutdown(void);

#endif /* GPSP_P4_NANO_NETWORK_H */
