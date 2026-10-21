/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include "gpsp_profile.h"

int main(void)
{
    struct gpsp_profile_window window;

    gpsp_profile_reset(10);
    gpsp_profile_record_frame(100, 7);
    gpsp_profile_record_frame(300, 3);
    gpsp_profile_add_frames(4);
    assert(gpsp_profile_frames() == 6);
    gpsp_profile_record_copy(12);
    gpsp_profile_record_send(9);
    gpsp_profile_take(30, &window);
    assert(window.start_us == 10 && window.end_us == 30);
    int receiver = CONFIG_GPSP_SERVER_HOST[0];

    if (CONFIG_GPSP_PERFORMANCE_DIAGNOSTICS || !CONFIG_GPSP_LCD ||
            CONFIG_GPSP_BENCHMARK || receiver)
    {
        assert(window.run.count == 2 && window.run.total_us == 400);
        assert(window.run.max_us == 300);
        assert(window.wait.count == 2 && window.wait.total_us == 10);
        assert(window.wait.max_us == 7);
    }
    else
        assert(window.run.count == 0 && window.wait.count == 0);
    assert(window.copy.count == 1 && window.copy.total_us == 12);
    assert(window.send.count == 1 && window.send.total_us == 9);
    gpsp_profile_take(40, &window);
    assert(window.start_us == 30 && window.run.count == 0);
    assert(window.copy.count == 0 && window.send.count == 0);
    gpsp_profile_reset(50);
    assert(gpsp_profile_frames() == 6);
#if CONFIG_GPSP_LCD_DSI_DIAGNOSTICS
    gpsp_profile_take_frame(60, &window);
    assert(window.run.count == 2 && window.run.total_us == 400);
    gpsp_profile_take_frame(70, &window);
    assert(window.start_us == 60 && window.run.count == 0);
#endif
#if CONFIG_GPSP_STATUS && CONFIG_GPSP_PERFORMANCE_DIAGNOSTICS
    struct gpsp_slow_frame slow = {.frame = 1, .run_us = 18000};
    struct gpsp_slow_window slow_window;

    gpsp_profile_record_slow(&slow);
    slow.frame = 2;
    slow.run_us = 22000;
    gpsp_profile_record_slow(&slow);
    gpsp_profile_take_slow(&slow_window);
    assert(slow_window.count == 2 && slow_window.total_us == 40000);
    assert(slow_window.worst.frame == 2 && slow_window.worst.run_us == 22000);
    gpsp_profile_take_slow(&slow_window);
    assert(slow_window.count == 0);
#endif
    return 0;
}
