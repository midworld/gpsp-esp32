# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

if("$ENV{GPSP_BUILD_ESPEMU}" STREQUAL "1")
    set(gpsp_network_requirements esp_eth esp_event esp_netif)
else()
    set(gpsp_network_requirements gpsp_devices gpsp_video)
endif()

if("$ENV{GPSP_BUILD_ESPEMU}" STREQUAL "1")
    set(gpsp_board_source "$ENV{GPSP_BOARD_DIR}/board.c")
else()
    set(gpsp_board_source "esp32p4/cache.c")
endif()

idf_component_register(
    SRCS ${gpsp_board_source}
        "gpsp_board.c"
        "transport.c"
        "gpsp_profile.c"
        "gpsp_rv32_platform.c"
        "gpsp_serial_input.c"
        "gpsp_status.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
        "../../protocol"
    REQUIRED_IDF_TARGETS
        esp32p4
        esp32s31
    REQUIRES gpsp_display
    PRIV_REQUIRES
        esp_driver_tsens
        esp_driver_uart
        esp_driver_usb_serial_jtag
        esp_mm
        esp_psram
        esp_timer
        gpsp
        heap
        lwip
        ${gpsp_network_requirements}
    LDFRAGMENTS
        "gpsp_memory.lf"
)
