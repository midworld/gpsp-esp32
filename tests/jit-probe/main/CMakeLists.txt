# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

idf_component_register(
    SRCS "jit_probe.c"
    PRIV_REQUIRES esp_mm esp_psram
)
