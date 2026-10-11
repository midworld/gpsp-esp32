/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "gpsp_board.h"
#include "gpsp_rv32_platform.h"
#include "gpsp_memory.h"

static bool lcd, audio, usb, status, serial, transport;
static bool menu_wifi;
static esp_err_t transport_result;
static esp_err_t peripheral_result;

static size_t psram_bytes;
static unsigned rom_limit;
const struct gpsp_rv32_platform_ops gpsp_esp_rv32_platform = {0};

void gpsp_rv32_set_platform(const struct gpsp_rv32_platform_ops *ops)
{
    assert(ops == &gpsp_esp_rv32_platform);
}

size_t esp_psram_get_size(void) { return psram_bytes; }
bool gpsp_set_rom_buffer_limit(unsigned mib)
{
    rom_limit = mib;
    return true;
}

static void test_rom_limit(void)
{
    const size_t sizes[] = {8 * 1024 * 1024, 16 * 1024 * 1024,
                           16 * 1024 * 1024 + 1, 32 * 1024 * 1024};
    const unsigned automatic[] = {4, 4, 8, 8};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        psram_bytes = sizes[i];
        gpsp_board_prepare_core();
        assert(rom_limit == (CONFIG_GPSP_ROM_BUFFER_SIZE ?
                CONFIG_GPSP_ROM_BUFFER_SIZE : automatic[i]));
    }
}

esp_err_t gpsp_p4_nano_lcd_init(void)
{
    lcd = true;
    return peripheral_result;
}

void gpsp_p4_nano_lcd_shutdown(void) { lcd = false; }

esp_err_t gpsp_p4_nano_audio_init(void)
{
    audio = peripheral_result == ESP_OK;
    return peripheral_result;
}

void gpsp_p4_nano_audio_shutdown(void)
{
    assert(!serial);
    audio = false;
}

int gpsp_p4_nano_audio_volume_step(int steps)
{
    assert(audio);
    return -6 + steps;
}

esp_err_t gpsp_p4_nano_usb_pad_init(void)
{
    usb = peripheral_result == ESP_OK;
    return peripheral_result;
}

void gpsp_p4_nano_usb_pad_shutdown(void) { usb = false; }
void gpsp_status_start(void) { status = true; }

void gpsp_status_stop(void)
{
    assert(!serial);
    status = false;
}

void gpsp_serial_input_start(void) { serial = true; }
void gpsp_serial_input_stop(void) { serial = false; }
uint32_t gpsp_serial_input_read(void) { return 0; }
uint32_t gpsp_p4_nano_usb_pad_read(void) { return 0; }
uint16_t gpsp_espemu_transport_read_input(void) { return 0; }

esp_err_t gpsp_espemu_transport_init(void)
{
    transport = transport_result == ESP_OK;
    return transport_result;
}

void gpsp_espemu_transport_shutdown(void) { transport = false; }
void gpsp_p4_nano_wifi_shutdown(void) { menu_wifi = false; }

static void assert_stopped(void)
{
    assert(!lcd && !audio && !usb && !status && !serial && !transport);
    assert(!menu_wifi);
    assert(gpsp_board_audio_volume_step(0) == INT_MIN);
}

int main(void)
{
    test_rom_limit();
    /* A fatal transport error must undo the peripherals already started. */
    transport_result = ESP_ERR_NO_MEM;
    assert(gpsp_board_init() == ESP_ERR_NO_MEM);
    assert_stopped();
    gpsp_board_shutdown();
    assert_stopped();

    /* A later retry must still initialize and stop normally. */
    transport_result = ESP_OK;
    assert(gpsp_board_init() == ESP_OK);
    assert(lcd && audio && usb && status && transport);
    assert(gpsp_board_audio_volume_step(0) == -6);
    gpsp_board_input_read();
    assert(serial);
    menu_wifi = true;
    gpsp_board_shutdown();
    assert_stopped();

    /* Optional peripherals may fail without stopping the application. */
    peripheral_result = ESP_FAIL;
    assert(gpsp_board_init() == ESP_OK);
    assert(!lcd && !audio && !usb && status && transport);
    assert(gpsp_board_audio_volume_step(0) == INT_MIN);
    gpsp_board_shutdown();
    assert_stopped();
    puts("board lifecycle: PASS");
    return 0;
}
