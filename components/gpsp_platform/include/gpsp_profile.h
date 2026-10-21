/* Frame timing counters shared by the frontend and transport tasks.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_PROFILE_H
#define GPSP_PROFILE_H

#include "sdkconfig.h"
#include <stdint.h>

struct gpsp_profile_phase
{
    uint32_t count;
    uint64_t total_us;
    uint32_t max_us;
};

struct gpsp_profile_window
{
    int64_t start_us;
    int64_t end_us;
    struct gpsp_profile_phase run;
    struct gpsp_profile_phase wait;
    struct gpsp_profile_phase copy;
    struct gpsp_profile_phase send;
};

void gpsp_profile_reset(int64_t now_us);
void gpsp_profile_record_frame(uint32_t run_us, uint32_t wait_us);
/* Core frames run beyond the recorded one, as in fast forward. Only the
   frame count grows; their time is in the recorded frame. */
void gpsp_profile_add_frames(uint32_t frames);
/* Frames recorded since boot; gpsp_profile_reset() keeps it. */
uint32_t gpsp_profile_frames(void);
void gpsp_profile_record_copy(uint32_t duration_us);
void gpsp_profile_record_send(uint32_t duration_us);
void gpsp_profile_take(int64_t now_us, struct gpsp_profile_window *window);
#if CONFIG_GPSP_STATUS && CONFIG_GPSP_PERFORMANCE_DIAGNOSTICS
struct gpsp_slow_frame
{
    uint32_t frame, run_us, render_us, translated, translate_us, upkeep_us;
    uint32_t rom_pages, rom_reloads, rom_remaps, rom_load_us;
};

struct gpsp_slow_window
{
    uint32_t count;
    uint64_t total_us;
    struct gpsp_slow_frame worst;
};

/* Keep the slowest frame's details; formatting runs on the status worker. */
void gpsp_profile_record_slow(const struct gpsp_slow_frame *frame);
void gpsp_profile_take_slow(struct gpsp_slow_window *window);
#endif
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
/* Independent frame window; does not consume transport measurements. */
void gpsp_profile_take_frame(int64_t now_us, struct gpsp_profile_window *window);
#endif

#endif /* GPSP_PROFILE_H */
