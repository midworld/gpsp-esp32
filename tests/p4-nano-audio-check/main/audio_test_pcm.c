/* PCM helpers for the P4 Nano audio check.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audio_test_pcm.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define AUDIO_TWO_PI 6.2831853071795864769f

int16_t audio_test_downmix(int16_t left, int16_t right)
{
    return (int16_t)(((int32_t)left + right) / 2);
}

static float audio_test_edge_gain(uint32_t position, uint32_t total,
        uint32_t fade)
{
    if (position >= total)
        return 0.0f;
    if (position < fade)
        return (float)position / fade;
    if (total - position < fade)
        return (float)(total - position) / fade;
    return 1.0f;
}

void audio_test_signal_next(struct audio_test_signal *signal,
        int16_t *left, int16_t *right)
{
    static const float melody[] = {
        261.6256f, 329.6276f, 391.9954f, 523.2511f,
        493.8833f, 391.9954f, 329.6276f, 293.6648f,
    };
    float progress = (float)signal->position / signal->total;
    float gain = audio_test_edge_gain(signal->position, signal->total,
            signal->rate / 200);
    float left_hz = signal->left_hz;
    float right_hz = signal->right_hz;
    float left_gain = 1.0f;
    float right_gain = 1.0f;

    switch (signal->pattern)
    {
        case AUDIO_PATTERN_SILENCE:
            gain = 0.0f;
            break;
        case AUDIO_PATTERN_SWEEP:
            left_hz = right_hz = 120.0f * powf(8000.0f / 120.0f, progress);
            break;
        case AUDIO_PATTERN_RAMP:
            gain *= 0.1f + 0.9f * (1.0f - fabsf(2.0f * progress - 1.0f));
            break;
        case AUDIO_PATTERN_PAN:
            left_gain = 1.0f - progress;
            right_gain = progress;
            break;
        case AUDIO_PATTERN_MELODY:
        {
            uint32_t note_frames = signal->rate / 4;
            unsigned note = signal->position / note_frames;
            left_hz = melody[note % 8];
            right_hz = melody[(note + 2) % 8] / 2.0f;
            gain *= audio_test_edge_gain(signal->position % note_frames,
                    note_frames, signal->rate / 200);
            break;
        }
        default:
            break;
    }

    *left = left_hz > 0.0f ? (int16_t)lroundf(
            sinf(signal->left_phase) * signal->amplitude * gain * left_gain) : 0;
    *right = right_hz > 0.0f ? (int16_t)lroundf(
            sinf(signal->right_phase) * signal->amplitude * gain * right_gain) : 0;
    if (signal->pattern == AUDIO_PATTERN_ANTIPHASE)
        *right = (int16_t)-*left;

    signal->left_phase += AUDIO_TWO_PI * left_hz / signal->rate;
    signal->right_phase += AUDIO_TWO_PI * right_hz / signal->rate;
    if (signal->left_phase >= AUDIO_TWO_PI)
        signal->left_phase -= AUDIO_TWO_PI;
    if (signal->right_phase >= AUDIO_TWO_PI)
        signal->right_phase -= AUDIO_TWO_PI;
    signal->position++;
}

void audio_test_resampler_init(struct audio_test_resampler *resampler,
        uint32_t input_rate)
{
    assert(input_rate >= AUDIO_TEST_RATE / AUDIO_TEST_RESAMPLE_MAX);
    memset(resampler, 0, sizeof(*resampler));
    resampler->input_rate = input_rate;
}

unsigned audio_test_resampler_push(struct audio_test_resampler *resampler,
        int16_t left, int16_t right, int16_t output[AUDIO_TEST_RESAMPLE_MAX])
{
    int16_t current = audio_test_downmix(left, right);
    unsigned count = 0;

    if (!resampler->primed)
    {
        resampler->previous = current;
        resampler->primed = true;
        return 0;
    }

    /* Integer phase preserves the rate across arbitrary callback boundaries. */
    while (resampler->phase < AUDIO_TEST_RATE)
    {
        int64_t delta = (int32_t)current - resampler->previous;
        assert(count < AUDIO_TEST_RESAMPLE_MAX);
        output[count++] = (int16_t)(resampler->previous +
                delta * resampler->phase / AUDIO_TEST_RATE);
        resampler->phase += resampler->input_rate;
    }
    resampler->phase -= AUDIO_TEST_RATE;
    resampler->previous = current;
    return count;
}

unsigned audio_test_gba_frames(uint32_t rate, uint64_t *remainder)
{
    unsigned frames;

    *remainder += (uint64_t)rate * AUDIO_TEST_GBA_CYCLES;
    frames = (unsigned)(*remainder / AUDIO_TEST_GBA_CLOCK);
    *remainder %= AUDIO_TEST_GBA_CLOCK;
    return frames;
}

bool audio_test_pcm_self_check(void)
{
    static const uint32_t rates[] = {32768, 44100, 48000, 65536};
    struct audio_test_resampler resampler;
    int16_t output[AUDIO_TEST_RESAMPLE_MAX];
    uint64_t remainder = 0;
    uint64_t frames = 0;
    unsigned i;

    if (audio_test_downmix(32767, 32767) != 32767 ||
        audio_test_downmix(-32768, -32768) != -32768 ||
        audio_test_downmix(3000, -3000) != 0 ||
        audio_test_downmix(3000, 0) != audio_test_downmix(0, 3000))
        return false;

    for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
    {
        uint32_t input;
        uint32_t count = 0;
        audio_test_resampler_init(&resampler, rates[i]);
        for (input = 0; input <= rates[i]; input++)
        {
            unsigned n = audio_test_resampler_push(&resampler, 1234, 1234, output);
            unsigned j;
            for (j = 0; j < n; j++)
                if (output[j] != 1234)
                    return false;
            count += n;
        }
        if (count != AUDIO_TEST_RATE || resampler.phase != 0)
            return false;
    }

    for (i = 0; i < 8; i++)
        frames += audio_test_gba_frames(32768, &remainder);
    return frames == 4389 && remainder == 0;
}
