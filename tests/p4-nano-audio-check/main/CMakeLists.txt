# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

idf_component_register(
    SRCS "audio_check.c" "audio_test_pcm.c" "audio_test_output.c"
    INCLUDE_DIRS "."
    REQUIRED_IDF_TARGETS esp32p4
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_pcnt esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    AUDIO_TEST_RATE=${CONFIG_AUDIO_TEST_OUTPUT_RATE}u)
