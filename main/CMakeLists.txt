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
        "gpsp_transfer.c"
        "gpsp_transfer_ui.c"
        "gpsp_ui.c"
        "gpsp_wifi_ui.c"
    PRIV_INCLUDE_DIRS
        "include"
    EMBED_TXTFILES
        "transfer.html"
    REQUIRED_IDF_TARGETS
        esp32p4
        esp32s31
    PRIV_REQUIRES
        gpsp_platform
        gpsp_devices
        esp_driver_uart
        esp_http_server
        esp_timer
        fatfs
        gpsp
        gpsp_font
        heap
        wear_levelling
)

set(GPSP_ROM_PATH "" CACHE STRING "GBA ROM to place in the FATFS image")
set(GPSP_SD_DIR "" CACHE STRING "Folder to place in the ESP-EMU SD card image")

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
elseif(NOT GPSP_SD_DIR)
    message(WARNING
        "GPSP_ROM_PATH is not set; the storage image will not be generated")
endif()

if(GPSP_SD_DIR)
    get_filename_component(gpsp_sd_dir "${GPSP_SD_DIR}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

    if(NOT CONFIG_ESPEMU OR NOT CONFIG_GPSP_SD_CARD)
        message(FATAL_ERROR "GPSP_SD_DIR needs CONFIG_ESPEMU and CONFIG_GPSP_SD_CARD")
    endif()
    if(NOT IS_DIRECTORY "${gpsp_sd_dir}")
        message(FATAL_ERROR "GPSP_SD_DIR is not a folder: ${gpsp_sd_dir}")
    endif()

    fatfs_create_spiflash_image(sdcard "${gpsp_sd_dir}" FLASH_IN_PROJECT)
endif()
