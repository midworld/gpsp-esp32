# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

project(gpsp_espemu_host LANGUAGES C)

find_package(SDL2 2.0.18 CONFIG QUIET)

if(TARGET SDL2::SDL2)
    set(GPSP_SDL2_TARGET SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    set(GPSP_SDL2_TARGET SDL2::SDL2-static)
elseif(NOT WIN32)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2>=2.0.18)
    set(GPSP_SDL2_TARGET PkgConfig::SDL2)
else()
    message(FATAL_ERROR
        "SDL2 was not found. Pass CMAKE_PREFIX_PATH or use a vcpkg toolchain.")
endif()

add_executable(gpsp-espemu-host
    src/main.c
    src/net.c
    src/serial.c
)

target_compile_features(gpsp-espemu-host PRIVATE c_std_11)
target_include_directories(gpsp-espemu-host PRIVATE
    src
    "${CMAKE_CURRENT_LIST_DIR}/../../protocol"
)
target_link_libraries(gpsp-espemu-host PRIVATE ${GPSP_SDL2_TARGET})

if(MSVC)
    target_compile_options(gpsp-espemu-host PRIVATE /W4)
    target_compile_definitions(gpsp-espemu-host PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(gpsp-espemu-host PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

if(WIN32)
    target_link_libraries(gpsp-espemu-host PRIVATE ws2_32)
endif()
