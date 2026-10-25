# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

if("$ENV{GPSP_BUILD_ESPEMU}" STREQUAL "1" OR
        "$ENV{GPSP_BUILD_BOARD}" STREQUAL "esp32s31")
    idf_component_register(INCLUDE_DIRS "include")
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
            "touch_st7123.c" "virtual_pad.c"
            "playstation_pad.c" "bt_pad.c" "usb_bt.c" "usb_bt_packet.c"
            "hid_gamepad.cc" "third_party/hid_report_parser/hid_report_parser.cpp"
            "$ENV{GPSP_BOARD_DIR}/board.c"
        INCLUDE_DIRS "include"
        REQUIRED_IDF_TARGETS esp32p4
        REQUIRES esp_driver_i2c
        PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2s
            esp_eth esp_event esp_mm esp_psram esp_timer esp_netif esp_wifi
            gpsp bt mbedtls nvs_flash fatfs sdmmc esp_driver_sdmmc
            ${gpsp_hosted_component} ${gpsp_wifi_remote_component} ${gpsp_usb_component}
    )
    # Unmodified upstream parser: keep its warnings out of the build.
    target_include_directories(${COMPONENT_LIB} SYSTEM PRIVATE "third_party/hid_report_parser")
    set_source_files_properties("third_party/hid_report_parser/hid_report_parser.cpp"
        PROPERTIES COMPILE_OPTIONS "-w")
    if(CONFIG_GPSP_AUDIO_HIGHPASS)
        target_sources(${COMPONENT_LIB} PRIVATE "audio_highpass.c")
    endif()
    if(CONFIG_GPSP_USB_PAD_DIAGNOSTICS)
        # Trace root-port events without changing the USB driver.
        idf_component_get_property(gpsp_usb_dir ${gpsp_usb_component} COMPONENT_DIR)
        target_sources(${COMPONENT_LIB} PRIVATE "usb_trace.c")
        target_include_directories(${COMPONENT_LIB} PRIVATE "${gpsp_usb_dir}/private_include")
        target_link_options(${COMPONENT_LIB} INTERFACE
            "-Wl,--wrap=hcd_port_handle_event"
            "-Wl,--undefined=__wrap_hcd_port_handle_event")
    endif()
endif()
