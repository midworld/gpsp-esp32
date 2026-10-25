# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "")

# Requirement expansion runs before Kconfig loads. Read a generated sdkconfig
# when present, or the defaults used to create it on a clean build.
if(DEFINED SDKCONFIG)
    set(gpsp_sdkconfig "${SDKCONFIG}")
else()
    set(gpsp_sdkconfig "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
endif()

if(NOT IS_ABSOLUTE "${gpsp_sdkconfig}")
    set(gpsp_sdkconfig "${CMAKE_CURRENT_LIST_DIR}/${gpsp_sdkconfig}")
endif()

set(gpsp_espemu_enabled FALSE)
if(EXISTS "${gpsp_sdkconfig}")
    file(STRINGS "${gpsp_sdkconfig}" gpsp_espemu_setting
        REGEX "^(CONFIG_ESPEMU=y|# CONFIG_ESPEMU is not set)$")
    if("${gpsp_espemu_setting}" STREQUAL "CONFIG_ESPEMU=y")
        set(gpsp_espemu_enabled TRUE)
    endif()
else()
    if(DEFINED SDKCONFIG_DEFAULTS)
        set(gpsp_defaults ${SDKCONFIG_DEFAULTS})
    else()
        set(gpsp_defaults "sdkconfig.defaults")
    endif()
    foreach(gpsp_default IN LISTS gpsp_defaults)
        if(NOT IS_ABSOLUTE "${gpsp_default}")
            set(gpsp_default "${CMAKE_CURRENT_LIST_DIR}/${gpsp_default}")
        endif()
        if(EXISTS "${gpsp_default}")
            file(STRINGS "${gpsp_default}" gpsp_espemu_setting
                REGEX "^(CONFIG_ESPEMU=y|# CONFIG_ESPEMU is not set)$")
            if("${gpsp_espemu_setting}" STREQUAL "CONFIG_ESPEMU=y")
                set(gpsp_espemu_enabled TRUE)
            elseif("${gpsp_espemu_setting}" STREQUAL
                    "# CONFIG_ESPEMU is not set")
                set(gpsp_espemu_enabled FALSE)
            endif()
        endif()
    endforeach()
endif()

if(NOT DEFINED GPSP_BOARD)
    if(gpsp_espemu_enabled)
        set(GPSP_BOARD espemu)
    else()
        set(GPSP_BOARD p4_nano)
    endif()
endif()
set(GPSP_BOARD "${GPSP_BOARD}" CACHE STRING "gpSP board")
set_property(CACHE GPSP_BOARD PROPERTY STRINGS p4_nano osptek_p4c5 wt9932p4x_tiny esp32s31 espemu)
if(NOT GPSP_BOARD MATCHES "^(p4_nano|osptek_p4c5|wt9932p4x_tiny|esp32s31|espemu)$")
    message(FATAL_ERROR "Unknown GPSP_BOARD: ${GPSP_BOARD}")
endif()
if(EXISTS "${gpsp_sdkconfig}")
    file(STRINGS "${gpsp_sdkconfig}" gpsp_saved_board
        REGEX "^CONFIG_GPSP_BOARD_NAME=\"[^\"]+\"$")
    if(gpsp_saved_board AND NOT gpsp_saved_board STREQUAL
            "CONFIG_GPSP_BOARD_NAME=\"${GPSP_BOARD}\"")
        message(FATAL_ERROR "sdkconfig belongs to another board; use a separate SDKCONFIG")
    endif()
endif()
if(DEFINED GPSP_CONFIGURED_BOARD AND NOT GPSP_CONFIGURED_BOARD STREQUAL GPSP_BOARD)
    message(FATAL_ERROR "Use a separate build directory and sdkconfig for each board")
endif()
set(GPSP_CONFIGURED_BOARD "${GPSP_BOARD}" CACHE INTERNAL "Configured board")
set(ENV{GPSP_BUILD_BOARD} "${GPSP_BOARD}")
set(ENV{GPSP_BOARD_DIR} "${CMAKE_CURRENT_LIST_DIR}/boards/${GPSP_BOARD}")
include("boards/${GPSP_BOARD}/board.cmake")
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;${gpsp_board_defaults}")
endif()
if(GPSP_BOARD STREQUAL "espemu")
    set(ENV{GPSP_BUILD_ESPEMU} "1")
else()
    set(ENV{GPSP_BUILD_ESPEMU} "0")
endif()
if(DEFINED ENV{GPSP_EXTRA_COMPONENT_DIRS})
    list(APPEND EXTRA_COMPONENT_DIRS $ENV{GPSP_EXTRA_COMPONENT_DIRS})
endif()

if(NOT DEFINED IDF_TARGET AND NOT GPSP_BOARD STREQUAL "espemu")
    set(IDF_TARGET esp32p4 CACHE STRING "IDF target")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(gpsp_esp32)

if(NOT CONFIG_GPSP_BOARD_NAME STREQUAL GPSP_BOARD)
    message(FATAL_ERROR "sdkconfig belongs to ${CONFIG_GPSP_BOARD_NAME}; use a separate SDKCONFIG for ${GPSP_BOARD}")
endif()
if((GPSP_BOARD STREQUAL "espemu" AND NOT CONFIG_ESPEMU) OR
        (NOT GPSP_BOARD STREQUAL "espemu" AND CONFIG_ESPEMU))
    message(FATAL_ERROR "GPSP_BOARD and CONFIG_ESPEMU disagree")
endif()
if(CONFIG_GPSP_USB_BT)
    # ESP-IDF 6.1 Bluedroid trips GCC string truncation checks.
    idf_component_get_property(gpsp_bt_lib bt COMPONENT_LIB)
    target_compile_options(${gpsp_bt_lib} PRIVATE -Wno-error=stringop-truncation)
    # Skip the optional packet type and duplicate link policy setup after
    # each link comes up; one adapter refused the packet type change.
    target_compile_definitions(${gpsp_bt_lib} PRIVATE BTM_BYPASS_EXTRA_ACL_SETUP=1)
endif()
