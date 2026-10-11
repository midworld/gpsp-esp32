/* Host checks for the audio test's PCM conversion.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "main/audio_test_pcm.h"

static void check_resampler(uint32_t rate)
{
    struct audio_test_resampler resampler;
    uint32_t input_frames = rate * 3;
    uint32_t output_frames = AUDIO_TEST_RATE * 3;
    int16_t *input = malloc(((size_t)input_frames + 1) * sizeof(*input));
    uint32_t position = 0;
    uint32_t output_position = 0;
    uint32_t seed = 1;

    assert(input);
    /* Large signed changes exercise the interpolation's wide accumulator. */
    for (position = 0; position <= input_frames; position++)
    {
        seed = seed * 1664525u + 1013904223u;
        input[position] = (int16_t)(seed >> 16);
    }
    audio_test_resampler_init(&resampler, rate);
    position = 0;
    while (position <= input_frames)
    {
        unsigned chunk = (position * 17u + 3u) % 1100u + 1u;
        unsigned i;

        /* Callback sizes change without resetting the converter's history. */
        for (i = 0; i < chunk && position <= input_frames; i++, position++)
        {
            int16_t output[AUDIO_TEST_RESAMPLE_MAX];
            unsigned n = audio_test_resampler_push(&resampler,
                    input[position], input[position], output);
            unsigned j;
            for (j = 0; j < n; j++, output_position++)
            {
                uint64_t time = (uint64_t)output_position * rate;
                uint32_t source = (uint32_t)(time / AUDIO_TEST_RATE);
                long double fraction = (long double)(time % AUDIO_TEST_RATE) /
                        AUDIO_TEST_RATE;
                long double reference;

                assert(output_position < output_frames);
                assert(source + 1 <= input_frames);
                reference = input[source] * (1.0L - fraction) +
                        input[source + 1] * fraction;
                assert(fabsl(output[j] - reference) < 1.01L);
            }
        }
    }
    assert(output_position == output_frames);
    free(input);
}

static void check_tone(uint32_t rate, float left_hz, float right_hz,
        unsigned expected_crossings)
{
    struct audio_test_signal signal = {
        .pattern = AUDIO_PATTERN_TONE,
        .rate = rate,
        .total = rate * 2,
        .amplitude = 3000,
        .left_hz = left_hz,
        .right_hz = right_hz,
    };
    struct audio_test_resampler resampler;
    unsigned crossings = 0;
    int16_t previous = 0;
    uint32_t count = 0;

    audio_test_resampler_init(&resampler, rate);
    while (signal.position <= signal.total)
    {
        int16_t left;
        int16_t right;
        int16_t output[AUDIO_TEST_RESAMPLE_MAX];
        unsigned n;
        unsigned j;

        audio_test_signal_next(&signal, &left, &right);
        n = audio_test_resampler_push(&resampler, left, right, output);
        for (j = 0; j < n; j++, count++)
        {
            if (previous <= 0 && output[j] > 0)
                crossings++;
            assert(abs(output[j]) <= 3000);
            previous = output[j];
        }
    }
    assert(count == AUDIO_TEST_RATE * 2);
    assert(abs((int)crossings - (int)expected_crossings) <= 1);
}

static void check_antiphase(void)
{
    struct audio_test_signal signal = {
        .pattern = AUDIO_PATTERN_ANTIPHASE,
        .rate = 32768,
        .total = 32768,
        .amplitude = 8000,
        .left_hz = 440,
        .right_hz = 440,
    };

    while (signal.position <= signal.total)
    {
        int16_t left;
        int16_t right;
        audio_test_signal_next(&signal, &left, &right);
        assert(audio_test_downmix(left, right) == 0);
    }
}

static void check_gba_batches(uint32_t rate)
{
    uint64_t remainder = 0;
    uint64_t total = 0;
    unsigned i;
    unsigned minimum = UINT32_MAX;
    unsigned maximum = 0;

    for (i = 0; i < 10000; i++)
    {
        unsigned frames = audio_test_gba_frames(rate, &remainder);
        total += frames;
        if (frames < minimum)
            minimum = frames;
        if (frames > maximum)
            maximum = frames;
    }
    assert(total == (uint64_t)rate * AUDIO_TEST_GBA_CYCLES * 10000 /
            AUDIO_TEST_GBA_CLOCK);
    assert(maximum - minimum == 1);
    if (rate == 32768)
        assert(minimum == 548 && maximum == 549);
    if (rate == 65536)
        assert(minimum == 1097 && maximum == 1098);
}

int main(void)
{
    static const uint32_t rates[] = {32768, 44100, 48000, 65536};
    unsigned i;

    assert(audio_test_pcm_self_check());
    for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
    {
        check_resampler(rates[i]);
        check_tone(rates[i], 440, 0, 880);
        check_tone(rates[i], 0, 880, 1760);
        check_tone(rates[i], 440, 440, 880);
        check_gba_batches(rates[i]);
    }
    check_antiphase();
    puts("PASS: conversion rates, signed interpolation, stereo routing, pitch, "
            "antiphase cancellation, and GBA frame counts");
    return 0;
}
