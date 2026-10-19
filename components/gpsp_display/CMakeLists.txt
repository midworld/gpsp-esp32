# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

idf_component_register(SRCS "gpsp_display.c" INCLUDE_DIRS "include")

if(CONFIG_GPSP_LCD_DSI_SIMD)
    target_sources(${COMPONENT_LIB} PRIVATE "gpsp_display_p4.S")
endif()
