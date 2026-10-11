# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

if("$ENV{GPSP_BUILD_ESPEMU}" STREQUAL "1")
    set(gpsp_network_requirements esp_eth esp_event esp_netif)
else()
    set(gpsp_network_requirements p4_nano)
endif()

idf_component_register(
    SRCS
        "espemu_board.c"
        "espemu_transport.c"
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
    PRIV_REQUIRES
        esp_driver_tsens
        esp_driver_uart
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
