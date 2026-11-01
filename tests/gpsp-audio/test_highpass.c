/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audio_highpass.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 32768u
#define BLOCK 352u
#define PI 3.14159265358979323846

/* The cutoffs the build accepts: 100 to 1000 Hz. */
static const unsigned s_cutoffs[] = {100, 300, 1000};

static void test_response(unsigned cutoff)
{
    static const unsigned frequencies[] = {100, 150, 300, 1000, 4000};
    int16_t samples[BLOCK];

    for (unsigned f = 0; f < sizeof(frequencies) / sizeof(frequencies[0]); f++)
    {
        struct audio_highpass filter;
        double input_power = 0.0;
        double output_power = 0.0;
        unsigned frequency = frequencies[f];

        audio_highpass_init(&filter, RATE, cutoff);
        for (unsigned position = 0; position < RATE * 2; position += BLOCK)
        {
            unsigned frames = RATE * 2 - position;

            if (frames > BLOCK)
                frames = BLOCK;
            for (unsigned i = 0; i < frames; i++)
            {
                samples[i] = (int16_t)(16000 * sin(2 * PI * frequency *
                        (position + i) / RATE));
                if (position + i >= RATE)
                    input_power += (double)samples[i] * samples[i];
            }
            audio_highpass_process(&filter, samples, frames);
            for (unsigned i = 0; i < frames; i++)
                if (position + i >= RATE)
                    output_power += (double)samples[i] * samples[i];
        }
        double gain = sqrt(output_power / input_power);
        double ratio = tan(PI * cutoff / RATE) / tan(PI * frequency / RATE);
        double expected = 1.0 / sqrt(1.0 + ratio * ratio * ratio * ratio);

        printf("%u Hz cutoff, %u Hz: %.3f dB\n", cutoff, frequency, 20 * log10(gain));
        assert(fabs(gain - expected) < 0.002);
    }
}

static void test_continuity(void)
{
    static int16_t whole[RATE];
    static int16_t chunks[RATE];
    static const unsigned sizes[] = {1, 17, BLOCK, 548, 549};
    struct audio_highpass first, second;

    for (unsigned i = 0; i < RATE; i++)
        whole[i] = (int16_t)(12000 * sin(2 * PI * 125 * i / RATE) +
                9000 * sin(2 * PI * 1000 * i / RATE));
    memcpy(chunks, whole, sizeof(whole));
    audio_highpass_init(&first, RATE, 300);
    audio_highpass_init(&second, RATE, 300);
    audio_highpass_process(&first, whole, RATE);
    for (unsigned position = 0, block = 0; position < RATE; block++)
    {
        unsigned frames = sizes[block % (sizeof(sizes) / sizeof(sizes[0]))];

        if (frames > RATE - position)
            frames = RATE - position;
        audio_highpass_process(&second, chunks + position, frames);
        position += frames;
    }
    assert(memcmp(whole, chunks, sizeof(whole)) == 0);

    audio_highpass_reset(&second);
    audio_highpass_init(&first, RATE, 300);
    int16_t a[BLOCK] = {16000};
    int16_t b[BLOCK] = {16000};

    audio_highpass_process(&first, a, BLOCK);
    audio_highpass_process(&second, b, BLOCK);
    assert(memcmp(a, b, sizeof(a)) == 0);
}

static void test_limits(unsigned cutoff)
{
    struct audio_highpass filter;
    int16_t samples[BLOCK];

    audio_highpass_init(&filter, RATE, cutoff);
    for (unsigned block = 0; block < 100; block++)
    {
        for (unsigned i = 0; i < BLOCK; i++)
            samples[i] = INT16_MAX;
        audio_highpass_process(&filter, samples, BLOCK);
    }
    for (unsigned i = 0; i < BLOCK; i++)
        assert(abs(samples[i]) <= 1);
    samples[0] = INT16_MIN;
    audio_highpass_process(&filter, samples, 1);
    assert(samples[0] == INT16_MIN);

    audio_highpass_reset(&filter);
    for (unsigned i = 0; i < BLOCK; i++)
        samples[i] = 0;
    audio_highpass_process(&filter, samples, BLOCK);
    for (unsigned i = 0; i < BLOCK; i++)
        assert(samples[i] == 0);
}

static void test_bypass(void)
{
    static int16_t samples[65536];
    static int16_t expected[65536];
    struct audio_highpass filter;

    for (unsigned i = 0; i < 65536; i++)
        samples[i] = (int16_t)((int)i + INT16_MIN);
    memcpy(expected, samples, sizeof(samples));
    audio_highpass_init(&filter, RATE, 0);
    audio_highpass_process(&filter, samples, 65536);
    assert(memcmp(samples, expected, sizeof(samples)) == 0);
}

int main(void)
{
    for (unsigned i = 0; i < sizeof(s_cutoffs) / sizeof(s_cutoffs[0]); i++)
    {
        test_response(s_cutoffs[i]);
        test_limits(s_cutoffs[i]);
    }
    test_continuity();
    test_bypass();
    puts("PASS: high-pass response, continuity, reset, clipping and bypass");
    return 0;
}
