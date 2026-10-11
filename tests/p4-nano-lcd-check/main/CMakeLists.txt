# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

idf_component_register(
    SRCS "lcd_check.c" "lcd_test_pattern.c"
    INCLUDE_DIRS "."
    REQUIRED_IDF_TARGETS esp32p4
    PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_lcd esp_timer heap
)
