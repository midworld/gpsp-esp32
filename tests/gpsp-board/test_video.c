/* Video buffer ownership with controlled display scheduling.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpsp_board.h"
#include "gpsp_lcd.h"
#include "gpsp_screen.h"

#define LCD_BUFFERS 3
#define LCD_FRAME_WIDTH 240
#define LCD_FRAME_HEIGHT 160

static uint16_t storage[LCD_BUFFERS][GPSP_SCREEN_BUFFER_BYTES / 2];
static uint16_t *s_buffers[LCD_BUFFERS] = {storage[0], storage[1], storage[2]};
static void *s_task = storage;
static unsigned notifications, transport_frames;
static uint16_t *core_owned, *core_current;
static bool transport_ok = true;

#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
static void xTaskNotifyGive(void *task)
{
    assert(task == s_task);
    notifications++;
}

/* The worker's state transitions are controlled by each test. */
#include "lcd_buffer_state.inc"
static enum lcd_buffer_state s_state[LCD_BUFFERS];
#include "lcd_buffers.inc"

bool gpsp_espemu_transport_submit_frame(const uint16_t *pixels,
        unsigned width, unsigned height, size_t pitch)
{
    assert(pixels && width == 240 && height == 160 && pitch == 480);
    transport_frames++;
    return transport_ok;
}

uint16_t *gpsp_screen_set_buffer(uint16_t *pixels)
{
    uint16_t *previous = core_current;
    if (pixels)
        core_current = pixels;
    return previous;
}

static void retro_unload_game(void)
{
    assert(core_current == core_owned);
    free(core_owned);
    core_owned = core_current = NULL;
}

static uint16_t *s_core_screen;
static const uint16_t *s_last_frame;
static size_t s_last_pitch;
static uint32_t s_frame_count;
#if CONFIG_GPSP_BENCHMARK
static const void *s_benchmark_pixels;
static unsigned s_benchmark_width, s_benchmark_height;
static size_t s_benchmark_pitch;
#elif CONFIG_GPSP_PERFORMANCE_DIAGNOSTICS
static uint32_t s_report_frame_count, s_report_run_count;
static int64_t s_report_start_us;
static const char *TAG = "test_video";
#define GPSP_PERFORMANCE_REPORT_INTERVAL_US 2000000
#define ESP_LOGI(...) video_log(__VA_ARGS__)
static void video_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
static int64_t esp_timer_get_time(void) { return 3000000; }
static uint32_t gpsp_profile_frames(void) { return s_frame_count; }
static uint32_t gpsp_frame_checksum(const void *pixels, unsigned width,
        unsigned height, size_t pitch)
{
    assert(pixels && width == 240 && height == 160 && pitch == 480);
    return 0;
}
#endif
#include "video_callbacks.inc"

static void load(void)
{
    assert(!core_owned && !core_current && !s_core_screen);
    core_owned = calloc(1, GPSP_SCREEN_BUFFER_BYTES);
    assert(core_owned);
    core_current = core_owned;
}

static void refresh(void)
{
    gpsp_video_refresh(core_current, 240, 160, 480);
}

static void all_free(void)
{
    for (unsigned i = 0; i < LCD_BUFFERS; i++)
        assert(s_state[i] == LCD_BUFFER_FREE);
}

static void check_release(void)
{
    uint16_t unrelated;
    gpsp_lcd_release_buffer(NULL);
    gpsp_lcd_release_buffer(&unrelated);
    for (unsigned state = LCD_BUFFER_FREE; state <= LCD_BUFFER_HELD; state++)
    {
        s_state[0] = state;
        gpsp_lcd_release_buffer(storage[0]);
        assert(s_state[0] == (state == LCD_BUFFER_WRITING ? LCD_BUFFER_FREE : state));
    }
    s_state[0] = LCD_BUFFER_FREE;
    all_free();
}

static void check_exhaustion(void)
{
    load();
    refresh();
    assert(s_core_screen == core_owned && core_current == storage[1]);
    s_state[0] = LCD_BUFFER_SENDING;
    uint16_t *held = gpsp_lcd_next_buffer();
    assert(held == storage[2]);
    core_current[0] = 0x1234;
    refresh();
    assert(core_current == core_owned && s_state[1] == LCD_BUFFER_READY);
    core_current[0] = 0x5678;
    transport_ok = false;
    unsigned sent = notifications;
    for (unsigned frame = 0; frame < 4; frame++)
    {
        refresh();
        assert(core_current == core_owned && s_core_screen == core_owned);
        assert(storage[0][0] == 0 && storage[1][0] == 0x1234);
        assert(notifications == sent);
    }
    transport_ok = true;
    gpsp_lcd_release_buffer(held);
    refresh();
    assert(core_current == storage[1] && storage[2][0] == 0x5678);
    assert(s_state[0] == LCD_BUFFER_SENDING && s_state[2] == LCD_BUFFER_READY);
    gpsp_unload_game();
    assert(!s_core_screen && s_state[1] == LCD_BUFFER_FREE);
    assert(s_state[0] == LCD_BUFFER_SENDING && s_state[2] == LCD_BUFFER_READY);
    s_state[0] = s_state[2] = LCD_BUFFER_FREE;
    all_free();
}

static void check_game_switches(void)
{
    for (unsigned game = 0; game < 32; game++)
    {
        load();
        if (game)
        {
            refresh();
            uint16_t *unused = core_current;
            unsigned frames = s_frame_count, sent = notifications;
            gpsp_video_refresh(NULL, 240, 160, 480);
            assert(core_current == unused && s_frame_count == frames);
            assert(notifications == sent);
            for (unsigned i = 0; i < LCD_BUFFERS; i++)
                if (s_state[i] == LCD_BUFFER_READY)
                    s_state[i] = LCD_BUFFER_FREE;
        }
        gpsp_unload_game();
        all_free();
    }
}

int main(void)
{
    check_release();
    check_exhaustion();
    check_game_switches();
    s_task = NULL;
    load();
    refresh();
    assert(core_current == core_owned && !s_core_screen);
    gpsp_unload_game();
    all_free();
    assert(transport_frames);
    printf("video ownership: PASS (benchmark=%u)\n", CONFIG_GPSP_BENCHMARK);
    return 0;
}
