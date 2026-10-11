/* Frame timing counters shared by the frontend and transport tasks.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_PROFILE_H
#define GPSP_PROFILE_H

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

#endif /* GPSP_PROFILE_H */
