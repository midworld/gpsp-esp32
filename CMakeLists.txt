# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards/espemu")

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

if(gpsp_espemu_enabled)
    set(ENV{GPSP_BUILD_ESPEMU} "1")
else()
    set(ENV{GPSP_BUILD_ESPEMU} "0")
    if(NOT DEFINED SDKCONFIG_DEFAULTS)
        set(SDKCONFIG_DEFAULTS
            "sdkconfig.defaults;sdkconfig.p4_nano.defaults")
    endif()
    list(APPEND EXTRA_COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/boards/p4_nano")
endif()
if(DEFINED ENV{GPSP_EXTRA_COMPONENT_DIRS})
    list(APPEND EXTRA_COMPONENT_DIRS $ENV{GPSP_EXTRA_COMPONENT_DIRS})
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(gpsp_esp32)
