# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

if("$ENV{GPSP_BUILD_ESPEMU}" STREQUAL "1")
    idf_component_register()
else()
    if("$ENV{IDF_COMPONENT_MANAGER}" STREQUAL "0")
        set(gpsp_hosted_component espressif__esp_hosted)
        set(gpsp_wifi_remote_component espressif__esp_wifi_remote)
        set(gpsp_usb_component espressif__usb)
    else()
        set(gpsp_hosted_component esp_hosted)
        set(gpsp_wifi_remote_component esp_wifi_remote)
        set(gpsp_usb_component usb)
    endif()

    idf_component_register(
        SRCS "audio_es8311.c" "i2c_bus.c" "network.c" "usb_pad.c" "sdmmc.c"
            "$ENV{GPSP_BOARD_DIR}/board.c"
        INCLUDE_DIRS "include"
        REQUIRED_IDF_TARGETS esp32p4
        REQUIRES esp_driver_i2c
        PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2s
            esp_eth esp_event esp_mm esp_psram esp_timer esp_netif esp_wifi
            gpsp mbedtls nvs_flash fatfs sdmmc esp_driver_sdmmc
            ${gpsp_hosted_component} ${gpsp_wifi_remote_component} ${gpsp_usb_component}
    )
endif()
