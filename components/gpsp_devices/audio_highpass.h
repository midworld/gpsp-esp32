/* Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_AUDIO_HIGHPASS_H
#define GPSP_AUDIO_HIGHPASS_H

#include <stddef.h>
#include <stdint.h>

struct audio_highpass
{
    float b0, a1, a2;
    float z1, z2;
};

/* Second-order Butterworth high-pass; zero cutoff bypasses the filter. */
void audio_highpass_init(struct audio_highpass *filter, unsigned rate,
        unsigned cutoff);
void audio_highpass_reset(struct audio_highpass *filter);
void audio_highpass_process(struct audio_highpass *filter, int16_t *samples,
        size_t frames);

#endif /* GPSP_AUDIO_HIGHPASS_H */
