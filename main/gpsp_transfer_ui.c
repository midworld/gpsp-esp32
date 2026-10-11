/* The File Transfer screen of the menu.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_transfer_ui.h"

#include "sdkconfig.h"

#if CONFIG_GPSP_MENU

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "gpsp_board.h"
#include "gpsp_save.h"
#include "gpsp_settings.h"
#include "gpsp_transfer.h"
#include "libretro.h"

#define TRANSFER_POLL_MS 16
/* The status lines are drawn again at most this often. */
#define TRANSFER_REDRAW_US 250000
#define TRANSFER_TEXT_X (GPSP_UI_PANEL_X + 10)
#define TRANSFER_VALUE_X (TRANSFER_TEXT_X + 9 * GPSP_UI_FONT_WIDTH)
#define TRANSFER_TOP (GPSP_UI_PANEL_Y + 8)
#define TRANSFER_FOOTER_Y \
    (GPSP_UI_PANEL_Y + GPSP_UI_PANEL_H - 8 - GPSP_UI_FONT_HEIGHT)
#define TRANSFER_BUTTON(id) (1u << RETRO_DEVICE_ID_JOYPAD_##id)

enum transfer_phase
{
    TRANSFER_STARTING,
    TRANSFER_JOINING,
    TRANSFER_OPENING_AP,
    TRANSFER_READY,
    TRANSFER_FAILED,
};

/* Written by the network task until it ends, then read by the screen. */
struct transfer_network
{
    atomic_int phase;
    atomic_bool running;
    atomic_bool cancel;
    bool access_point;
    char ssid[33];
    char password[65];
    char address[16];
};

static const char *TAG = "gpsp_transfer_ui";

static struct transfer_network s_network;
static struct gpsp_settings_wifi s_wifi;

/* An access point named after the chip, with a fresh password. */
static void transfer_ap_name(char *ssid, size_t ssid_size, char *password,
        size_t password_size)
{
    static const char letters[] = "abcdefghjkmnpqrstuvwxyz23456789";
    uint8_t mac[6] = {0};

    esp_efuse_mac_get_default(mac);
    snprintf(ssid, ssid_size, "gpSP-%02X%02X", mac[4], mac[5]);
    for (size_t i = 0; i < 8 && i + 1 < password_size; i++)
        password[i] = letters[esp_random() % (sizeof(letters) - 1)];
    password[8 < password_size ? 8 : password_size - 1] = '\0';
}

static void transfer_network_task(void *arg)
{
    struct transfer_network *network = arg;
    char psk[65];
    esp_err_t result = gpsp_board_wifi_open();

    if (result == ESP_OK && s_wifi.ssid[0] &&
            !atomic_load(&network->cancel))
    {
        atomic_store(&network->phase, TRANSFER_JOINING);
        /* A typed password comes before a kept key. */
        result = gpsp_board_wifi_connect(s_wifi.ssid,
                s_wifi.password[0] ? s_wifi.password : s_wifi.psk,
                network->address, sizeof(network->address), psk, sizeof(psk));
        memset(psk, 0, sizeof(psk));
        if (result == ESP_OK)
            memcpy(network->ssid, s_wifi.ssid, sizeof(network->ssid));
    }
    else if (result == ESP_OK)
        result = ESP_ERR_NOT_FOUND;

    /* Without a saved network that answers, the board is the network. */
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE &&
            !atomic_load(&network->cancel))
    {
        atomic_store(&network->phase, TRANSFER_OPENING_AP);
        transfer_ap_name(network->ssid, sizeof(network->ssid),
                network->password, sizeof(network->password));
        network->access_point = true;
        result = gpsp_board_wifi_start_ap(network->ssid, network->password,
                network->address, sizeof(network->address));
    }
    if (result == ESP_OK && !atomic_load(&network->cancel))
    {
        result = gpsp_transfer_start();
        if (result == ESP_OK && network->access_point)
            ESP_LOGI(TAG, "File transfer: http://%s/ (access point %s, "
                    "password %s)", network->address, network->ssid,
                    network->password);
        else if (result == ESP_OK)
            ESP_LOGI(TAG, "File transfer: http://%s/ (network %s)",
                    network->address, network->ssid);
    }
    atomic_store(&network->phase,
            result == ESP_OK ? TRANSFER_READY : TRANSFER_FAILED);
    atomic_store(&network->running, false);
    vTaskDelete(NULL);
}

static void transfer_line(uint16_t *pixels, int row, const char *label,
        const char *value, uint16_t color)
{
    int y = TRANSFER_TOP + (row + 2) * GPSP_UI_LINE;

    gpsp_ui_text(pixels, TRANSFER_TEXT_X, y, label, 9, GPSP_UI_COLOR_DIM);
    gpsp_ui_text(pixels, TRANSFER_VALUE_X, y, value, 28, color);
}

static void transfer_draw(uint16_t *pixels, const uint16_t *frame,
        const struct gpsp_transfer_status *status)
{
    static const char *const phases[] = {
        [TRANSFER_STARTING] = "Starting Wi-Fi...",
        [TRANSFER_JOINING] = "Joining the saved network...",
        [TRANSFER_OPENING_AP] = "Opening an access point...",
        [TRANSFER_READY] = "Ready",
        [TRANSFER_FAILED] = "Wi-Fi did not start",
    };
    int phase = atomic_load(&s_network.phase);
    char text[80];

    gpsp_ui_backdrop(pixels, frame);
    gpsp_ui_panel(pixels);
    gpsp_ui_text(pixels, TRANSFER_TEXT_X, TRANSFER_TOP, "File Transfer", 37,
            GPSP_UI_COLOR_TITLE);
    gpsp_ui_text(pixels, TRANSFER_TEXT_X, TRANSFER_TOP + GPSP_UI_LINE,
            phases[phase], 37, GPSP_UI_COLOR_TEXT);

    if (phase == TRANSFER_READY)
    {
        int row = 0;

        transfer_line(pixels, row++, s_network.access_point ? "Wi-Fi" :
                "Network", s_network.ssid, GPSP_UI_COLOR_TEXT);
        if (s_network.access_point)
            transfer_line(pixels, row++, "Password", s_network.password,
                    GPSP_UI_COLOR_TITLE);
        snprintf(text, sizeof(text), "http://%s/", s_network.address);
        transfer_line(pixels, row++, "Open", text, GPSP_UI_COLOR_TITLE);
        row++;
        if (status->file[0])
        {
            unsigned percent = status->total ?
                    (unsigned)((uint64_t)status->done * 100 / status->total) : 0;

            snprintf(text, sizeof(text), "%3u%% %s", percent, status->file);
            transfer_line(pixels, row++, status->upload ? "Upload" :
                    "Download", text, GPSP_UI_COLOR_TEXT);
        }
        else
            transfer_line(pixels, row++, "Now", "Idle", GPSP_UI_COLOR_DIM);
        snprintf(text, sizeof(text), "%u in, %u out", status->uploads,
                status->downloads);
        transfer_line(pixels, row++, "Done", text, GPSP_UI_COLOR_TEXT);
    }

    gpsp_ui_text(pixels, TRANSFER_TEXT_X, TRANSFER_FOOTER_Y,
            status->game_changed ? "B: Stop and restart the game" : "B: Stop",
            37, status->game_changed ? GPSP_UI_COLOR_TITLE : GPSP_UI_COLOR_DIM);
    gpsp_board_ui_draw(0, GPSP_UI_HEIGHT);
}

enum gpsp_ui_exit gpsp_transfer_ui_run(uint16_t *pixels,
        const uint16_t *frame, bool *game_changed)
{
    struct gpsp_transfer_status status = {0};
    struct gpsp_transfer_status shown = {0};
    enum gpsp_ui_exit exit = GPSP_UI_BACK;
    int shown_phase = -1;
    int64_t redraw_us = 0;
    uint32_t held;

    *game_changed = false;
#if CONFIG_GPSP_CARTRIDGE_AUTOSAVE && \
        !CONFIG_GPSP_BENCHMARK_STOP_AFTER_REPORT && \
        !CONFIG_GPSP_BENCHMARK_FRESH_SAVE
    gpsp_save_wait();
#endif
    gpsp_settings_wifi(&s_wifi);
    memset(&s_network, 0, sizeof(s_network));
    atomic_store(&s_network.phase, TRANSFER_STARTING);
    atomic_store(&s_network.running, true);
    if (xTaskCreate(transfer_network_task, "gpsp_transfer_net", 4096,
            &s_network, tskIDLE_PRIORITY + 2, NULL) != pdPASS)
    {
        atomic_store(&s_network.running, false);
        atomic_store(&s_network.phase, TRANSFER_FAILED);
    }

    held = gpsp_board_input_read();
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(TRANSFER_POLL_MS));
        uint32_t input = gpsp_board_input_read();
        uint32_t pressed = input & ~held;
        int64_t now_us = esp_timer_get_time();
        int phase = atomic_load(&s_network.phase);

        held = input;
        if (gpsp_ui_poll_command(pixels))
        {
            exit = GPSP_UI_QUIT;
            break;
        }
        if (pressed & GPSP_BOARD_INPUT_HOME)
        {
            exit = GPSP_UI_RESUME;
            break;
        }
        if (pressed & TRANSFER_BUTTON(B))
            break;

        gpsp_transfer_status(&status);
        if ((phase != shown_phase ||
                memcmp(&status, &shown, sizeof(status))) &&
                now_us >= redraw_us)
        {
            transfer_draw(pixels, frame, &status);
            shown = status;
            shown_phase = phase;
            redraw_us = now_us + TRANSFER_REDRAW_US;
        }
    }

    /* The network task ends first; a join stops at once. */
    atomic_store(&s_network.cancel, true);
    while (atomic_load(&s_network.running))
    {
        gpsp_board_wifi_cancel();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    gpsp_transfer_stop();
    gpsp_board_wifi_close();
    gpsp_transfer_status(&status);
    *game_changed = status.game_changed;
    return exit;
}

#endif
