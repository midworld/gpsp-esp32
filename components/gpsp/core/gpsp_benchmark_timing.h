/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Exclusive core timing for benchmark builds. */
#ifndef GPSP_BENCHMARK_TIMING_H
#define GPSP_BENCHMARK_TIMING_H

#include "gpsp_benchmark_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(GPSP_BENCHMARK)
void gpsp_benchmark_timing_start(uint64_t (*now)(void));
void gpsp_benchmark_timing_stop(struct gpsp_benchmark_timing *out);
void gpsp_benchmark_timing_enter(enum gpsp_benchmark_timing_bucket bucket);
void gpsp_benchmark_timing_leave(void);
#if defined(GPSP_BENCHMARK_PHASE_WORK)
void gpsp_benchmark_timing_dma_bytes(uint32_t bytes);
void gpsp_benchmark_timing_sound_direct(uint32_t samples, int active);
void gpsp_benchmark_timing_sound_psg(uint32_t samples);
#endif

#if defined(GPSP_BENCHMARK_VIDEO_DETAIL)
enum gpsp_benchmark_video_phase
{
  GPSP_BENCHMARK_VIDEO_OAM,
  GPSP_BENCHMARK_VIDEO_LAYERS,
  GPSP_BENCHMARK_VIDEO_DRAW,
  GPSP_BENCHMARK_VIDEO_AFFINE,
  GPSP_BENCHMARK_VIDEO_TEXT_BG,
  GPSP_BENCHMARK_VIDEO_AFFINE_BG,
  GPSP_BENCHMARK_VIDEO_BITMAP_BG,
  GPSP_BENCHMARK_VIDEO_OBJ_LAYER,
  GPSP_BENCHMARK_VIDEO_EFFECTS
};

void gpsp_benchmark_video_detail_get(struct gpsp_benchmark_video_detail *out);
uint32_t gpsp_benchmark_video_detail_now(void);
void gpsp_benchmark_video_detail_add(enum gpsp_benchmark_video_phase phase,
                                     uint32_t cycles);
void gpsp_benchmark_video_detail_line(unsigned mode, int forced_blank, int window);
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif
