/* Per-frame gpSP timing reported to benchmark frontends.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_BENCHMARK_TYPES_H
#define GPSP_BENCHMARK_TYPES_H

#include <stdint.h>

enum gpsp_benchmark_timing_bucket
{
    GPSP_BENCHMARK_CPU,
    GPSP_BENCHMARK_SCHED,
    GPSP_BENCHMARK_TIMER,
    GPSP_BENCHMARK_DMA,
    GPSP_BENCHMARK_SOUND,
    GPSP_BENCHMARK_SCANLINE,
    GPSP_BENCHMARK_VIDEO_SETUP,
    GPSP_BENCHMARK_TIMING_BUCKET_COUNT
};

struct gpsp_benchmark_timing
{
    uint64_t cycles[GPSP_BENCHMARK_TIMING_BUCKET_COUNT];
    uint64_t instret[GPSP_BENCHMARK_TIMING_BUCKET_COUNT];
    uint32_t enters[GPSP_BENCHMARK_TIMING_BUCKET_COUNT];
    uint64_t dma_bytes;
    uint64_t direct_samples; /* Interleaved buffer slots, two per stereo frame. */
    uint64_t direct_active_samples;
    uint64_t psg_samples; /* Stereo frames. */
    uint32_t direct_calls;
    uint32_t direct_active_calls;
    uint32_t psg_calls;
};

struct gpsp_benchmark_video_detail
{
    uint64_t oam_cycles;
    uint64_t layers_cycles;
    uint64_t draw_cycles; /* Rendering or forced blank fill. */
    uint64_t affine_cycles;
    uint64_t text_bg_cycles;
    uint64_t affine_bg_cycles;
    uint64_t bitmap_bg_cycles;
    uint64_t obj_layer_cycles;
    uint64_t effects_cycles;
    uint32_t oam_updates;
    uint32_t forced_blank_lines;
    uint32_t window_lines; /* Window rendering actually ran. */
    uint32_t mode_lines[6]; /* Processed lines, including forced blank. */
};

#endif /* GPSP_BENCHMARK_TYPES_H */
