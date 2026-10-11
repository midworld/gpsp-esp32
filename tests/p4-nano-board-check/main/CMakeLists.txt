# SPDX-License-Identifier: GPL-3.0-or-later

idf_component_register(
    SRCS
        "board_check.c" "iperf3_tcp.c" "sample_wifi.c"
    PRIV_REQUIRES
        esp_hosted esp_wifi_remote esp_wifi esp_netif esp_event
        esp_psram spi_flash nvs_flash esp_timer esp_hw_support iperf3
)
