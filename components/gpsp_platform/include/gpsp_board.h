/* gpSP board interface.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_BOARD_H
#define GPSP_BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"
#include "gpsp_display.h"

/**
 * Initialize the selected board backend.
 * Releases started services on failure.
 *
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
esp_err_t gpsp_board_init(void);

/* Prepare the calling task for core execution. Call before retro_init(). */
void gpsp_board_prepare_core(void);

#if CONFIG_GPSP_BENCHMARK
/* A benchmark starts only after the output receiver is connected. */
bool gpsp_board_ready(void);

struct gpsp_board_output_stats
{
    uint32_t submitted;
    uint32_t sent;
    uint32_t dropped;
};

void gpsp_board_output_stats(struct gpsp_board_output_stats *stats);
#endif

/**
 * Give one RGB565 frame to the selected board backend.
 *
 * A buffer from gpsp_board_video_buffer() passes to the backend. Any other
 * buffer is valid only until this function returns; an asynchronous backend
 * must copy it before returning.
 *
 * @param pixels Pointer to the first RGB565 pixel.
 * @param width Frame width in pixels.
 * @param height Frame height in pixels.
 * @param pitch Number of bytes between two source rows.
 * @return true if the frame was accepted, or false if it was dropped.
 */
bool gpsp_board_video_present(
        const uint16_t *pixels, unsigned width, unsigned height, size_t pitch);

/**
 * Return a buffer for the core to render the next frame into, so that the
 * backend can show it without a copy.
 *
 * @return A GPSP_SCREEN_BUFFER_BYTES buffer, or NULL to keep the current one.
 */
uint16_t *gpsp_board_video_buffer(void);

/**
 * Set the frameskip used for FPS warning colors.
 *
 * @param frameskip Frames skipped after each drawn one; below 0 when it
 *        is automatic.
 */
void gpsp_board_video_frameskip(int frameskip);

/* Display effects are available only on the DSI backend. */
bool gpsp_board_display_options(enum gpsp_display_filter *filter,
        enum gpsp_display_color *color);
void gpsp_board_set_display_options(enum gpsp_display_filter filter,
        enum gpsp_display_color color);

/* Bit above the RetroPad buttons: the pad's home button. */
#define GPSP_BOARD_INPUT_HOME (1u << 16)

/**
 * Read the current RetroPad state for port 0.
 *
 * @return A bitmask whose bit positions are libretro RetroPad button IDs,
 *         plus GPSP_BOARD_INPUT_HOME.
 */
uint32_t gpsp_board_input_read(void);

#if CONFIG_GPSP_MENU
/* Full-screen UI while the game is paused. */
#define GPSP_BOARD_UI_WIDTH 320
#define GPSP_BOARD_UI_HEIGHT 240

/**
 * Stop showing game frames and take the display for the UI.
 *
 * @param pixels Set to a GPSP_BOARD_UI_WIDTH x GPSP_BOARD_UI_HEIGHT RGB565
 *               picture for the UI to draw in.
 * @param frame Set to the last game frame (240x160, pitch 240 pixels), or
 *              NULL if there is none.
 * Both stay valid until gpsp_board_ui_end().
 * @return false without a display.
 */
bool gpsp_board_ui_begin(uint16_t **pixels, const uint16_t **frame);

/**
 * Show rows y0 to y1 (exclusive) of the UI picture. Returns after the rows
 * are sent, so the caller may change the picture again.
 */
void gpsp_board_ui_draw(unsigned y0, unsigned y1);

/** Clear the UI and show game frames again. */
void gpsp_board_ui_end(void);

/* Wi-Fi for the menu: it starts only when asked, never at boot. */
#define GPSP_BOARD_WIFI_SCAN_MAX 24

struct gpsp_board_wifi_ap
{
    char ssid[33];
    int8_t rssi;
    bool secure;
};

/** Start Wi-Fi without joining a network; takes a few seconds. */
esp_err_t gpsp_board_wifi_open(void);

/** Scan for access points, strongest first, one per name. */
size_t gpsp_board_wifi_scan(struct gpsp_board_wifi_ap *aps, size_t max);

/**
 * Join a network and wait for an address.
 *
 * @param password A passphrase, or a WPA2 key as 64 hex digits.
 * @param address Set to the IPv4 address on success.
 * @param psk Set to the WPA2 key of a passphrase that joined a WPA2
 *        network, as 64 hex digits; empty otherwise. At least 65 bytes.
 * @return ESP_ERR_NOT_FOUND without the network, ESP_ERR_INVALID_ARG when
 *         the password is refused, ESP_ERR_TIMEOUT without an answer.
 */
esp_err_t gpsp_board_wifi_connect(const char *ssid, const char *password,
        char *address, size_t size, char *psk, size_t psk_size);

/** End a join in progress from another task; it returns
    ESP_ERR_INVALID_STATE. */
void gpsp_board_wifi_cancel(void);

/**
 * Turn the open radio into a WPA2 access point; the next open makes it a
 * station again.
 *
 * @param address Set to the access point's IPv4 address.
 */
esp_err_t gpsp_board_wifi_start_ap(const char *ssid, const char *password,
        char *address, size_t size);

/** Stop the radio; the next open starts it again quickly. */
void gpsp_board_wifi_close(void);
#endif

enum gpsp_board_command
{
    GPSP_BOARD_COMMAND_NONE = 0,
    GPSP_BOARD_COMMAND_SAVE_STATE = 1,
    GPSP_BOARD_COMMAND_LOAD_STATE = 2,
    /* Same values as the protocol's commands. */
    GPSP_BOARD_COMMAND_QUIT = 6,
    GPSP_BOARD_COMMAND_SCREENSHOT = 8,
};

/* Session zero is serial; TCP requests keep their connection and ID. */
struct gpsp_board_request
{
    enum gpsp_board_command command;
    uint32_t session;
    uint32_t id;
};

/* Keep valid SAVE/LOAD requests until reported; discard disconnected ones. */
struct gpsp_board_request gpsp_board_command_read(void);
bool gpsp_board_command_valid(struct gpsp_board_request request);
void gpsp_board_command_report(struct gpsp_board_request request, bool success);

/**
 * Give stereo audio frames to the selected board backend.
 *
 * The sample buffer is valid only until this function returns. Samples are
 * signed 16-bit values in interleaved left-right order.
 *
 * @param samples Pointer to the first interleaved stereo sample.
 * @param frames Number of stereo frames in the sample buffer.
 * @return The number of stereo frames accepted by the backend.
 */
size_t gpsp_board_audio_write(const int16_t *samples, size_t frames);

/**
 * How full the audio backend's queue is against its target: -1 when
 * empty, 0 on target, up to +1. Zero without an audio backend.
 */
float gpsp_board_audio_balance(void);

/**
 * The audio queue right now as a multiple of its target (no averaging).
 * Zero without an audio backend.
 */
float gpsp_board_audio_fill(void);

/**
 * Move the speaker volume by steps of 3 dB; zero steps reads it.
 *
 * @return The volume in dB, or INT_MIN without a speaker.
 */
int gpsp_board_audio_volume_step(int steps);

void gpsp_board_shutdown(void);

#endif /* GPSP_BOARD_H */
