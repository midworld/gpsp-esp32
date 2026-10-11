/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Exclusive core timing for benchmark builds. */
#include "gpsp_benchmark_timing.h"

#if defined(GPSP_BENCHMARK)
#include <string.h>

#define GPSP_BENCHMARK_TIMING_STACK_SIZE 16

static struct gpsp_benchmark_timing timing;
static uint64_t (*timing_now)(void);
static uint32_t last_tick;
#if defined(GPSP_BENCHMARK_INSTRET)
static uint32_t last_instret;
static uint32_t (*instret_now)(void);

void gpsp_benchmark_set_instret_counter(uint32_t (*now)(void))
{
  instret_now = now;
}

static uint32_t read_instret(void)
{
  return instret_now ? instret_now() : 0;
}
#endif
static uint8_t stack[GPSP_BENCHMARK_TIMING_STACK_SIZE];
static unsigned depth;
static unsigned overflow_depth;
static enum gpsp_benchmark_timing_bucket current;
static int recording;
#if defined(GPSP_BENCHMARK_VIDEO_DETAIL)
static struct gpsp_benchmark_video_detail video_detail;
#endif

static void timing_advance(void)
{
  uint32_t tick = (uint32_t)timing_now();
  timing.cycles[current] += (uint32_t)(tick - last_tick);
  last_tick = tick;
#if defined(GPSP_BENCHMARK_INSTRET)
  uint32_t instret = read_instret();
  timing.instret[current] += (uint32_t)(instret - last_instret);
  last_instret = instret;
#endif
}

void gpsp_benchmark_timing_start(uint64_t (*now)(void))
{
  memset(&timing, 0, sizeof(timing));
#if defined(GPSP_BENCHMARK_VIDEO_DETAIL)
  memset(&video_detail, 0, sizeof(video_detail));
#endif
  timing_now = now;
  depth = 0;
  overflow_depth = 0;
  current = GPSP_BENCHMARK_CPU;
  recording = now != 0;
  last_tick = recording ? (uint32_t)now() : 0;
#if defined(GPSP_BENCHMARK_INSTRET)
  last_instret = read_instret();
#endif
}

void gpsp_benchmark_timing_stop(struct gpsp_benchmark_timing *out)
{
  if (recording)
    timing_advance();
  recording = 0;
  timing_now = 0;
  *out = timing;
}

void gpsp_benchmark_timing_enter(enum gpsp_benchmark_timing_bucket bucket)
{
  if (!recording || bucket >= GPSP_BENCHMARK_TIMING_BUCKET_COUNT)
    return;
  if (depth == GPSP_BENCHMARK_TIMING_STACK_SIZE || overflow_depth)
  {
    overflow_depth++;
    return;
  }
  timing_advance();
#if defined(GPSP_BENCHMARK_PHASE_WORK)
  timing.enters[bucket]++;
#endif
  stack[depth++] = (uint8_t)current;
  current = bucket;
}

void gpsp_benchmark_timing_leave(void)
{
  if (!recording)
    return;
  if (overflow_depth)
  {
    overflow_depth--;
    return;
  }
  if (!depth)
    return;
  timing_advance();
  current = (enum gpsp_benchmark_timing_bucket)stack[--depth];
}

#if defined(GPSP_BENCHMARK_PHASE_WORK)
void gpsp_benchmark_timing_dma_bytes(uint32_t bytes)
{
  if (recording)
    timing.dma_bytes += bytes;
}

void gpsp_benchmark_timing_sound_direct(uint32_t samples, int active)
{
  if (recording)
  {
    timing.direct_calls++;
    timing.direct_samples += samples;
    if (active)
    {
      timing.direct_active_calls++;
      timing.direct_active_samples += samples;
    }
  }
}

void gpsp_benchmark_timing_sound_psg(uint32_t samples)
{
  if (recording)
  {
    timing.psg_calls++;
    timing.psg_samples += samples;
  }
}
#endif

#if defined(GPSP_BENCHMARK_VIDEO_DETAIL)
void gpsp_benchmark_video_detail_get(struct gpsp_benchmark_video_detail *out)
{
  *out = video_detail;
}

uint32_t gpsp_benchmark_video_detail_now(void)
{
  return recording ? (uint32_t)timing_now() : 0;
}

void gpsp_benchmark_video_detail_add(enum gpsp_benchmark_video_phase phase,
                                     uint32_t cycles)
{
  if (!recording)
    return;

  switch (phase)
  {
    case GPSP_BENCHMARK_VIDEO_OAM:
      video_detail.oam_cycles += cycles;
      video_detail.oam_updates++;
      break;
    case GPSP_BENCHMARK_VIDEO_LAYERS:
      video_detail.layers_cycles += cycles;
      break;
    case GPSP_BENCHMARK_VIDEO_DRAW:
      video_detail.draw_cycles += cycles;
      break;
    case GPSP_BENCHMARK_VIDEO_AFFINE:
      video_detail.affine_cycles += cycles;
      break;
    case GPSP_BENCHMARK_VIDEO_TEXT_BG:
      video_detail.text_bg_cycles += cycles;
      break;
    case GPSP_BENCHMARK_VIDEO_AFFINE_BG:
      video_detail.affine_bg_cycles += cycles;
      break;
    case GPSP_BENCHMARK_VIDEO_BITMAP_BG:
      video_detail.bitmap_bg_cycles += cycles;
      break;
    case GPSP_BENCHMARK_VIDEO_OBJ_LAYER:
      video_detail.obj_layer_cycles += cycles;
      break;
    case GPSP_BENCHMARK_VIDEO_EFFECTS:
      video_detail.effects_cycles += cycles;
      break;
  }
}

void gpsp_benchmark_video_detail_line(unsigned mode, int forced_blank, int window)
{
  if (!recording)
    return;

  if (mode < 6)
    video_detail.mode_lines[mode]++;
  if (forced_blank)
    video_detail.forced_blank_lines++;
  else if (window)
    video_detail.window_lines++;
}
#endif
#endif
