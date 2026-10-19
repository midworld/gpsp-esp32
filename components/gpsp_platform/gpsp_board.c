/* Board backend for gpSP.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_board.h"

#include <limits.h>
#include <string.h>

#include "esp_log.h"
#include "esp_psram.h"
#include "gpsp_memory.h"
#include "gpsp_transport.h"
#if CONFIG_GPSP_DCACHE_AUTOLOAD
#include "gpsp_cache.h"
#endif
#if CONFIG_GPSP_LCD
#include "gpsp_lcd.h"
#endif
#if CONFIG_GPSP_LCD_DSI
#include "gpsp_lcd_dsi.h"
#endif
#if CONFIG_GPSP_AUDIO
#include "gpsp_audio.h"
#endif
#if CONFIG_GPSP_SERIAL_INPUT
#include "gpsp_serial_input.h"
#endif
#if CONFIG_GPSP_USB_PAD
#include "gpsp_usb_pad.h"
#endif
#if CONFIG_GPSP_STATUS
#include "gpsp_status.h"
#endif
#include "gpsp_rv32_esp_platform.h"
#if CONFIG_GPSP_MENU && !CONFIG_ESPEMU
#include "gpsp_network.h"
#endif
#if CONFIG_GPSP_MENU && CONFIG_ESPEMU
#include "esp_heap_caps.h"
#include "espemu_protocol.h"
#endif

#if CONFIG_GPSP_AUDIO
static bool s_audio_ready;
#endif
#if CONFIG_GPSP_MENU && CONFIG_ESPEMU
/* The menu screen goes to the host; the core keeps the last frame while
   the menu runs. */
static uint16_t *s_ui_pixels;
static const uint16_t *s_last_frame;
static bool s_ui_shown;
/* Connection that has the current picture; zero for none. */
static uint32_t s_ui_session;

/* The menu draws only on changes: a new or failed connection gets the
   picture again from the input poll. */
static void gpsp_espemu_ui_send(void)
{
    uint32_t session = gpsp_espemu_transport_session();

    s_ui_session = session && gpsp_espemu_transport_submit_frame(s_ui_pixels,
            GPSP_ESPEMU_UI_WIDTH, GPSP_ESPEMU_UI_HEIGHT,
            GPSP_ESPEMU_UI_WIDTH * 2) ? session : 0;
}
#endif

esp_err_t gpsp_board_init(void)
{
    esp_err_t result;

#if CONFIG_GPSP_DCACHE_AUTOLOAD
    gpsp_cache_init();
#endif
    gpsp_rv32_set_platform(&gpsp_esp_rv32_platform);
#if CONFIG_GPSP_LCD
    /* Keep running if the display cannot be initialized. */
    if (gpsp_lcd_init() != ESP_OK)
        gpsp_lcd_shutdown();
#endif
#if CONFIG_GPSP_AUDIO
    /* Without a working codec the game still runs, silently. */
    s_audio_ready = gpsp_audio_init() == ESP_OK;
#endif
#if CONFIG_GPSP_USB_PAD
    /* Other input sources still work without USB. */
    gpsp_usb_pad_init();
#endif
#if CONFIG_GPSP_STATUS
    gpsp_status_start();
#endif
    result = gpsp_espemu_transport_init();
    if (result != ESP_OK)
        gpsp_board_shutdown();
    return result;
}

void gpsp_board_prepare_core(void)
{
    unsigned rom_mib = CONFIG_GPSP_ROM_BUFFER_SIZE;
    if (!rom_mib)
        rom_mib = esp_psram_get_size() > 16 * 1024 * 1024 ? 8 : 4;
    ESP_ERROR_CHECK(gpsp_set_rom_buffer_limit(rom_mib) ?
            ESP_OK : ESP_ERR_INVALID_STATE);
    ESP_LOGI("gpsp_board", "ROM buffer limit: %u MiB", rom_mib);

#if CONFIG_GPSP_AFFINE_PIE
    /* Allocate the task's PIE save area before the first frame. */
    __asm__ volatile(
            ".option push\n"
            ".option arch, +xespv\n"
            "esp.zero.q q0\n"
            ".option pop\n"
            ::: "memory");
#endif
}

#if CONFIG_GPSP_BENCHMARK
bool gpsp_board_ready(void)
{
    return gpsp_espemu_transport_connected();
}

void gpsp_board_output_stats(struct gpsp_board_output_stats *stats)
{
    gpsp_espemu_transport_get_output_stats(
            &stats->submitted, &stats->sent, &stats->dropped);
}
#endif

bool gpsp_board_video_present(
        const uint16_t *pixels, unsigned width, unsigned height, size_t pitch)
{
#if CONFIG_GPSP_LCD
    gpsp_lcd_present(pixels, width, height, pitch);
#endif
#if CONFIG_GPSP_MENU && CONFIG_ESPEMU
    s_last_frame = width == GPSP_ESPEMU_SCREEN_WIDTH &&
            height == GPSP_ESPEMU_SCREEN_HEIGHT &&
            pitch == GPSP_ESPEMU_SCREEN_PITCH ? pixels : NULL;
#endif
    return gpsp_espemu_transport_submit_frame(
            pixels, width, height, pitch);
}

uint16_t *gpsp_board_video_buffer(void)
{
#if CONFIG_GPSP_LCD
    return gpsp_lcd_next_buffer();
#else
    return NULL;
#endif
}

void gpsp_board_video_frameskip(int frameskip)
{
#if CONFIG_GPSP_LCD
    gpsp_lcd_frameskip(frameskip);
#else
    (void)frameskip;
#endif
}

bool gpsp_board_display_options(enum gpsp_display_filter *filter,
        enum gpsp_display_color *color)
{
#if CONFIG_GPSP_LCD_DSI
    gpsp_dsi_options(filter, color);
    return true;
#else
    *filter = GPSP_DISPLAY_NEAREST;
    *color = GPSP_DISPLAY_COLOR_NONE;
    return false;
#endif
}

void gpsp_board_set_display_options(enum gpsp_display_filter filter,
        enum gpsp_display_color color)
{
#if CONFIG_GPSP_LCD_DSI
    gpsp_dsi_set_options(filter, color);
#else
    (void)filter;
    (void)color;
#endif
}

bool gpsp_board_stats(void)
{
#if CONFIG_GPSP_LCD_DSI
    return gpsp_dsi_stats_enabled();
#else
    return false;
#endif
}

void gpsp_board_set_stats(bool enabled)
{
#if CONFIG_GPSP_LCD_DSI
    gpsp_dsi_set_stats(enabled);
#else
    (void)enabled;
#endif
}

uint32_t gpsp_board_input_read(void)
{
    uint32_t input = gpsp_espemu_transport_read_input();

#if CONFIG_GPSP_MENU && CONFIG_ESPEMU
    if (s_ui_shown && s_ui_session != gpsp_espemu_transport_session())
        gpsp_espemu_ui_send();
#endif
#if CONFIG_GPSP_SERIAL_INPUT
    /* Started on the first poll: boot reads the ROM name on the same UART. */
    gpsp_serial_input_start();
    input |= gpsp_serial_input_read();
#endif
#if CONFIG_GPSP_USB_PAD
    input |= gpsp_usb_pad_read();
#endif
    return input;
}

#if CONFIG_GPSP_MENU && CONFIG_ESPEMU
bool gpsp_board_ui_begin(uint16_t **pixels, const uint16_t **frame)
{
    if (!s_ui_pixels)
        s_ui_pixels = heap_caps_malloc(GPSP_ESPEMU_UI_FRAME_BYTES,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    *pixels = s_ui_pixels;
    *frame = s_last_frame;
    return s_ui_pixels != NULL;
}

void gpsp_board_ui_draw(unsigned y0, unsigned y1)
{
    /* The host gets the whole screen; frames are small next to TCP. */
    (void)y0;
    (void)y1;
    s_ui_shown = true;
    gpsp_espemu_ui_send();
}

void gpsp_board_ui_end(void)
{
    s_ui_shown = false;
}

esp_err_t gpsp_board_wifi_open(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

size_t gpsp_board_wifi_scan(struct gpsp_board_wifi_ap *aps, size_t max)
{
    (void)aps;
    (void)max;
    return 0;
}

esp_err_t gpsp_board_wifi_connect(const char *ssid, const char *password,
        char *address, size_t size, char *psk, size_t psk_size)
{
    (void)ssid;
    (void)password;
    (void)address;
    (void)size;
    (void)psk;
    (void)psk_size;
    return ESP_ERR_NOT_SUPPORTED;
}

void gpsp_board_wifi_cancel(void)
{
}

esp_err_t gpsp_board_wifi_start_ap(const char *ssid, const char *password,
        char *address, size_t size)
{
    (void)ssid;
    (void)password;
    (void)address;
    (void)size;
    return ESP_ERR_NOT_SUPPORTED;
}

void gpsp_board_wifi_close(void)
{
}
#elif CONFIG_GPSP_MENU
bool gpsp_board_ui_begin(uint16_t **pixels, const uint16_t **frame)
{
    return gpsp_lcd_ui_begin(pixels, frame);
}

void gpsp_board_ui_draw(unsigned y0, unsigned y1)
{
    gpsp_lcd_ui_draw(y0, y1);
}

void gpsp_board_ui_end(void)
{
    gpsp_lcd_ui_end();
}

esp_err_t gpsp_board_wifi_open(void)
{
    return gpsp_wifi_open();
}

_Static_assert(sizeof(((struct gpsp_board_wifi_ap *)0)->ssid) ==
        sizeof(((struct gpsp_wifi_ap *)0)->ssid), "SSID size");

size_t gpsp_board_wifi_scan(struct gpsp_board_wifi_ap *aps, size_t max)
{
    struct gpsp_wifi_ap found[GPSP_BOARD_WIFI_SCAN_MAX];
    size_t count;

    if (max > GPSP_BOARD_WIFI_SCAN_MAX)
        max = GPSP_BOARD_WIFI_SCAN_MAX;
    count = gpsp_wifi_scan(found, max);
    for (size_t i = 0; i < count; i++)
    {
        memcpy(aps[i].ssid, found[i].ssid, sizeof(aps[i].ssid));
        aps[i].ssid[sizeof(aps[i].ssid) - 1] = '\0';
        aps[i].rssi = found[i].rssi;
        aps[i].secure = found[i].secure;
    }
    return count;
}

esp_err_t gpsp_board_wifi_connect(const char *ssid, const char *password,
        char *address, size_t size, char *psk, size_t psk_size)
{
    return gpsp_wifi_connect(ssid, password, address, size, psk,
            psk_size);
}

void gpsp_board_wifi_cancel(void)
{
    gpsp_wifi_cancel();
}

esp_err_t gpsp_board_wifi_start_ap(const char *ssid, const char *password,
        char *address, size_t size)
{
    return gpsp_wifi_start_ap(ssid, password, address, size);
}

void gpsp_board_wifi_close(void)
{
    gpsp_wifi_close();
}
#endif

struct gpsp_board_request gpsp_board_command_read(void)
{
    struct gpsp_board_request request = gpsp_espemu_transport_read_command();

#if CONFIG_GPSP_SERIAL_INPUT
    if (request.command == GPSP_BOARD_COMMAND_NONE)
        request.command =
                (enum gpsp_board_command)gpsp_serial_input_read_command();
#endif
    return request;
}

bool gpsp_board_command_valid(struct gpsp_board_request request)
{
    return !request.session || gpsp_espemu_transport_command_valid(request);
}

void gpsp_board_command_report(struct gpsp_board_request request, bool success)
{
    if (request.session)
        gpsp_espemu_transport_report_command(request, success);
#if CONFIG_GPSP_SERIAL_INPUT
    else
        gpsp_serial_input_report_command((uint16_t)request.command, success);
#endif
}

size_t gpsp_board_audio_write(const int16_t *samples, size_t frames)
{
#if CONFIG_GPSP_AUDIO
    return gpsp_audio_write(samples, frames);
#else
    (void)samples;
    (void)frames;
    return 0;
#endif
}

float gpsp_board_audio_balance(void)
{
#if CONFIG_GPSP_AUDIO
    return gpsp_audio_balance();
#else
    return 0.0f;
#endif
}

float gpsp_board_audio_fill(void)
{
#if CONFIG_GPSP_AUDIO
    return gpsp_audio_fill();
#else
    return 0.0f;
#endif
}

int gpsp_board_audio_volume_step(int steps)
{
#if CONFIG_GPSP_AUDIO
    if (s_audio_ready)
        return gpsp_audio_volume_step(steps);
#else
    (void)steps;
#endif
    return INT_MIN;
}

void gpsp_board_shutdown(void)
{
#if CONFIG_GPSP_SERIAL_INPUT
    /* Stop commands before the services they use. */
    gpsp_serial_input_stop();
#endif
#if CONFIG_GPSP_STATUS
    gpsp_status_stop();
#endif
#if CONFIG_GPSP_AUDIO
    s_audio_ready = false;
    gpsp_audio_shutdown();
#endif
#if CONFIG_GPSP_USB_PAD
    gpsp_usb_pad_shutdown();
#endif
#if CONFIG_GPSP_LCD
    gpsp_lcd_shutdown();
#endif
    gpsp_espemu_transport_shutdown();
#if CONFIG_GPSP_MENU && !CONFIG_ESPEMU
    gpsp_wifi_shutdown();
#endif
}
