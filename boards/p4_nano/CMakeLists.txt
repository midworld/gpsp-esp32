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
        SRCS
            "p4_nano_audio.c"
            "p4_nano_cache.c"
            "p4_nano_lcd.c"
            "p4_nano_network.c"
            "p4_nano_usb_pad.c"
        INCLUDE_DIRS "include"
        REQUIRED_IDF_TARGETS esp32p4
        PRIV_REQUIRES
            esp_driver_gpio
            esp_driver_i2c
            esp_driver_i2s
            esp_driver_spi
            esp_eth
            esp_event
            esp_lcd
            esp_psram
            esp_timer
            ${gpsp_hosted_component}
            esp_netif
            esp_wifi
            gpsp
            gpsp_font
            mbedtls
            ${gpsp_wifi_remote_component}
            nvs_flash
            ${gpsp_usb_component}
    )
endif()
