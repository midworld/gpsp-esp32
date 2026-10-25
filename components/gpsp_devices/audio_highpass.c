/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audio_highpass.h"

#include <math.h>

void audio_highpass_reset(struct audio_highpass *filter)
{
    filter->z1 = filter->z2 = 0.0f;
}

void audio_highpass_init(struct audio_highpass *filter, unsigned rate,
        unsigned cutoff)
{
    *filter = (struct audio_highpass){0};
    if (!cutoff)
        return;

    float k = tanf(3.14159265358979323846f * cutoff / rate);
    float norm = 1.0f / (1.0f + 1.4142135623730951f * k + k * k);

    filter->b0 = norm;
    filter->a1 = 2.0f * (k * k - 1.0f) * norm;
    filter->a2 = (1.0f - 1.4142135623730951f * k + k * k) * norm;
}

void audio_highpass_process(struct audio_highpass *filter, int16_t *samples,
        size_t frames)
{
    if (filter->b0 == 0.0f)
        return;

    for (size_t i = 0; i < frames; i++)
    {
        float input = samples[i];
        float output = filter->b0 * input + filter->z1;

        /* Keep the feedback state unclipped. */
        filter->z1 = -2.0f * filter->b0 * input - filter->a1 * output +
                filter->z2;
        filter->z2 = filter->b0 * input - filter->a2 * output;
        samples[i] = output > INT16_MAX ? INT16_MAX :
                output < INT16_MIN ? INT16_MIN : (int16_t)output;
    }
}
