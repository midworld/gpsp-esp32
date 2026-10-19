# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)
set(GPSP_BOARD p4_nano CACHE STRING "Test board")
if(NOT GPSP_BOARD MATCHES "^(p4_nano|osptek_p4c5)$")
    message(FATAL_ERROR "Unsupported DSI test board")
endif()
set(ENV{GPSP_BUILD_BOARD} "${GPSP_BOARD}")
set(EXTRA_COMPONENT_DIRS "../../components/gpsp_font" "../../components/gpsp_display")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(p4_nano_dsi_check)
