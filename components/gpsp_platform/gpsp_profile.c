/* Frame timing counters shared by the frontend and transport tasks.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpsp_profile.h"

#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_profile_lock = portMUX_INITIALIZER_UNLOCKED;
static struct gpsp_profile_window s_window;
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
static struct gpsp_profile_window s_frame_window;
#endif
static atomic_uint s_frames;
#if CONFIG_GPSP_STATUS
static struct gpsp_slow_window s_slow;

void gpsp_profile_record_slow(const struct gpsp_slow_frame *frame)
{
    portENTER_CRITICAL(&s_profile_lock);
    s_slow.count++;
    s_slow.total_us += frame->run_us;
    if (frame->run_us > s_slow.worst.run_us)
        s_slow.worst = *frame;
    portEXIT_CRITICAL(&s_profile_lock);
}

void gpsp_profile_take_slow(struct gpsp_slow_window *window)
{
    portENTER_CRITICAL(&s_profile_lock);
    *window = s_slow;
    memset(&s_slow, 0, sizeof(s_slow));
    portEXIT_CRITICAL(&s_profile_lock);
}
#endif

static void gpsp_profile_record(
        struct gpsp_profile_phase *phase, uint32_t duration_us)
{
    phase->count++;
    phase->total_us += duration_us;

    if (duration_us > phase->max_us)
        phase->max_us = duration_us;
}

void gpsp_profile_reset(int64_t now_us)
{
    portENTER_CRITICAL(&s_profile_lock);
    memset(&s_window, 0, sizeof(s_window));
    s_window.start_us = now_us;
    portEXIT_CRITICAL(&s_profile_lock);
}

void gpsp_profile_record_frame(uint32_t run_us, uint32_t wait_us)
{
    portENTER_CRITICAL(&s_profile_lock);
    gpsp_profile_record(&s_window.run, run_us);
    gpsp_profile_record(&s_window.wait, wait_us);
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    gpsp_profile_record(&s_frame_window.run, run_us);
    gpsp_profile_record(&s_frame_window.wait, wait_us);
#endif
    portEXIT_CRITICAL(&s_profile_lock);
    atomic_fetch_add(&s_frames, 1);
}

void gpsp_profile_add_frames(uint32_t frames)
{
    atomic_fetch_add(&s_frames, frames);
}

uint32_t gpsp_profile_frames(void)
{
    return atomic_load(&s_frames);
}

void gpsp_profile_record_copy(uint32_t duration_us)
{
    portENTER_CRITICAL(&s_profile_lock);
    gpsp_profile_record(&s_window.copy, duration_us);
    portEXIT_CRITICAL(&s_profile_lock);
}

void gpsp_profile_record_send(uint32_t duration_us)
{
    portENTER_CRITICAL(&s_profile_lock);
    gpsp_profile_record(&s_window.send, duration_us);
    portEXIT_CRITICAL(&s_profile_lock);
}

void gpsp_profile_take(int64_t now_us, struct gpsp_profile_window *window)
{
    portENTER_CRITICAL(&s_profile_lock);
    s_window.end_us = now_us;
    *window = s_window;
    memset(&s_window, 0, sizeof(s_window));
    s_window.start_us = now_us;
    portEXIT_CRITICAL(&s_profile_lock);
}

#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
void gpsp_profile_take_frame(int64_t now_us, struct gpsp_profile_window *window)
{
    portENTER_CRITICAL(&s_profile_lock);
    s_frame_window.end_us = now_us;
    *window = s_frame_window;
    memset(&s_frame_window, 0, sizeof(s_frame_window));
    s_frame_window.start_us = now_us;
    portEXIT_CRITICAL(&s_profile_lock);
}
#endif
