/* PCM helpers for the P4 Nano audio check.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIO_TEST_PCM_H
#define AUDIO_TEST_PCM_H

#include <stdbool.h>
#include <stdint.h>

#ifndef AUDIO_TEST_RATE
#define AUDIO_TEST_RATE 48000u
#endif
#define AUDIO_TEST_BLOCK_FRAMES (AUDIO_TEST_RATE / 100u)
#define AUDIO_TEST_RESAMPLE_MAX 2u
#define AUDIO_TEST_GBA_CYCLES 280896u
#define AUDIO_TEST_GBA_CLOCK 16777216u

enum audio_test_pattern
{
    AUDIO_PATTERN_TONE,
    AUDIO_PATTERN_SILENCE,
    AUDIO_PATTERN_SWEEP,
    AUDIO_PATTERN_RAMP,
    AUDIO_PATTERN_PAN,
    AUDIO_PATTERN_MELODY,
    AUDIO_PATTERN_ANTIPHASE,
};

struct audio_test_signal
{
    enum audio_test_pattern pattern;
    uint32_t rate;
    uint32_t total;
    uint32_t position;
    int amplitude;
    float left_hz;
    float right_hz;
    float left_phase;
    float right_phase;
};

struct audio_test_resampler
{
    uint32_t input_rate;
    uint32_t phase;
    int16_t previous;
    bool primed;
};

int16_t audio_test_downmix(int16_t left, int16_t right);
void audio_test_signal_next(struct audio_test_signal *signal,
        int16_t *left, int16_t *right);
void audio_test_resampler_init(struct audio_test_resampler *resampler,
        uint32_t input_rate);
/* Feed one lookahead sample after the last input frame to finish its interval. */
unsigned audio_test_resampler_push(struct audio_test_resampler *resampler,
        int16_t left, int16_t right, int16_t output[AUDIO_TEST_RESAMPLE_MAX]);
unsigned audio_test_gba_frames(uint32_t rate, uint64_t *remainder);
bool audio_test_pcm_self_check(void);

#endif /* AUDIO_TEST_PCM_H */
