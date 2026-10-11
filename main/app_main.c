/* gpSP ESP-IDF application entry point.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

#include "gpsp_board.h"
#include "gpsp_frontend.h"
#include "gpsp_menu.h"
#include "gpsp_settings.h"
#include "gpsp_storage.h"
#include "libretro.h"

static const char *TAG = "gpsp";

#if CONFIG_GPSP_SD_CARD && CONFIG_GPSP_MENU
/* No ROM was named: the menu runs without a game until Game List picks
   one. */
static esp_err_t gpsp_pick_rom(void)
{
    enum gpsp_menu_result result;

    /* The menu shows the settings, such as whether the auto state loads. */
    gpsp_settings_load();
    result = gpsp_menu_run(false);
    if (result == GPSP_MENU_QUIT)
        ESP_LOGI(TAG, "Game closed by request");
    return result == GPSP_MENU_SWITCH ? gpsp_storage_use_requested_rom() :
            ESP_ERR_NOT_FOUND;
}
#endif

void app_main(void)
{
    struct retro_system_info system_info = {0};
    unsigned api_version = retro_api_version();
    esp_err_t result;

    if (api_version != RETRO_API_VERSION)
    {
        ESP_LOGE(TAG, "Unsupported libretro API version: %u", api_version);
        return;
    }

    result = gpsp_board_init();

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Board initialization failed: %s",
                esp_err_to_name(result));
        return;
    }

    retro_get_system_info(&system_info);
    ESP_LOGI(TAG, "Core linked: %s %s", system_info.library_name,
            system_info.library_version);

    ESP_LOGI(TAG, "Memory: %u KiB internal, %u KiB PSRAM free",
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    result = gpsp_storage_mount();

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Storage initialization failed: %s",
                esp_err_to_name(result));
        gpsp_board_shutdown();
        return;
    }

#if CONFIG_GPSP_SD_CARD && CONFIG_GPSP_MENU
    if (!gpsp_storage_rom_path())
    {
        result = gpsp_pick_rom();
        if (result != ESP_OK)
        {
            gpsp_storage_unmount();
            gpsp_board_shutdown();
            return;
        }
    }
#endif

#if CONFIG_GPSP_SD_CARD && CONFIG_GPSP_MENU
    bool switched = false;
#endif

    for (;;)
    {
        result = gpsp_frontend_run(gpsp_storage_rom_path());
        if (result == ESP_OK)
            ESP_LOGI(TAG, "Frontend completed");
        else
            ESP_LOGE(TAG, "Frontend stopped: %s", esp_err_to_name(result));
#if CONFIG_GPSP_SD_CARD && CONFIG_GPSP_MENU
        /* A ROM that fails after a switch may only lack memory: try once
           more from a clean restart. */
        if (result != ESP_OK && switched && !gpsp_storage_rom_requested())
        {
            gpsp_storage_request_rom(gpsp_storage_rom_path(),
                    gpsp_storage_skip_auto_load());
            gpsp_storage_unmount();
            gpsp_board_shutdown();
            ESP_LOGI(TAG, "Restarting into the chosen ROM");
            esp_restart();
        }
        /* A ROM picked from the game list starts in place. A ROM gone since
           then brings the game list back. */
        if (!gpsp_storage_rom_requested())
            break;
        ESP_LOGI(TAG, "Switching to the chosen ROM");
        if (gpsp_storage_use_requested_rom() != ESP_OK &&
                gpsp_pick_rom() != ESP_OK)
            break;
        switched = true;
#else
        break;
#endif
    }

    gpsp_storage_unmount();
    gpsp_board_shutdown();
}
