# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

if("$ENV{GPSP_BUILD_ESPEMU}" STREQUAL "1")
    idf_component_register()
else()
    idf_component_register(
        SRCS "lcd.c" "spi.c" "dsi.c" "dsi_hud.c" "dsi_pad.c"
            "panels/waveshare_dsi.c" "panels/osptek_ydp430bt009.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "."
        REQUIRED_IDF_TARGETS esp32p4
        REQUIRES gpsp_display
        PRIV_REQUIRES gpsp_devices gpsp gpsp_font esp_driver_gpio
            esp_driver_spi esp_driver_ppa esp_driver_dma esp_lcd esp_mm esp_timer
    )
    if(CONFIG_GPSP_LCD_DSI_SIMD AND CONFIG_GPSP_LCD_DSI_DMA_EXPAND)
        target_sources(${COMPONENT_LIB} PRIVATE "dsi_hud_p4.S")
    endif()
endif()
