# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

set(IDF_TARGET esp32s31 CACHE STRING "IDF target")
if(NOT IDF_TARGET STREQUAL "esp32s31")
    message(FATAL_ERROR "The esp32s31 board requires IDF_TARGET=esp32s31")
endif()
set(gpsp_board_defaults "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
