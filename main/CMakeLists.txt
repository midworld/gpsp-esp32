# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT "$ENV{GPSP_BUILD_ESPEMU}" STREQUAL "1" AND
        NOT IDF_TARGET STREQUAL "esp32p4")
    message(FATAL_ERROR
        "Physical gpSP backend supports esp32p4 only; set CONFIG_ESPEMU=y for ESP-EMU")
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "gpsp_frontend.c"
        "gpsp_keyboard.c"
        "gpsp_loader.c"
        "gpsp_menu.c"
        "gpsp_png.c"
        "gpsp_save.c"
        "gpsp_save_file.c"
        "gpsp_screenshot.c"
        "gpsp_settings.c"
        "gpsp_state.c"
        "gpsp_storage.c"
        "gpsp_ui.c"
        "gpsp_wifi_ui.c"
    PRIV_INCLUDE_DIRS
        "include"
    REQUIRED_IDF_TARGETS
        esp32p4
        esp32s31
    PRIV_REQUIRES
        espemu
        esp_driver_sdmmc
        esp_driver_uart
        esp_mm
        esp_timer
        fatfs
        gpsp
        gpsp_font
        heap
        sdmmc
        wear_levelling
)

set(GPSP_ROM_PATH "" CACHE STRING "GBA ROM to place in the FATFS image")

if(GPSP_ROM_PATH)
    get_filename_component(gpsp_rom_path "${GPSP_ROM_PATH}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

    if(NOT EXISTS "${gpsp_rom_path}")
        message(FATAL_ERROR "GPSP_ROM_PATH does not exist: ${gpsp_rom_path}")
    endif()

    set(gpsp_storage_dir "${CMAKE_CURRENT_BINARY_DIR}/storage")
    file(MAKE_DIRECTORY "${gpsp_storage_dir}")
    configure_file("${gpsp_rom_path}" "${gpsp_storage_dir}/game.gba" COPYONLY)
    fatfs_create_spiflash_image(storage "${gpsp_storage_dir}" FLASH_IN_PROJECT)
else()
    message(WARNING
        "GPSP_ROM_PATH is not set; the storage image will not be generated")
endif()
