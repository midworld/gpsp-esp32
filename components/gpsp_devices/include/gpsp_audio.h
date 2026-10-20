/* Speaker output through the ES8311 DAC.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPSP_AUDIO_H
#define GPSP_AUDIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

/* The core's sound rate; the I2S clock runs at the same rate. */
#define GPSP_AUDIO_RATE 32768u

esp_err_t gpsp_audio_init(void);

/**
 * Queue interleaved stereo frames; the speaker plays their mono mix.
 *
 * @return Frames accepted. A full queue drops the rest.
 */
size_t gpsp_audio_write(const int16_t *stereo, size_t frames);

/**
 * Queue level against its target: -1 empty, 0 on target, +1 at twice.
 * Averaged over about 160 ms, since sound arrives a frame at a time.
 */
float gpsp_audio_balance(void);

/** Queue level right now as a multiple of the target; 0 without audio. */
float gpsp_audio_fill(void);

/**
 * Move the DAC volume by steps of 3 dB, between -60 and 0 dB.
 * Zero steps reads the current volume.
 *
 * @return The new volume in dB.
 */
int gpsp_audio_volume_step(int steps);

/* Mute the DAC without changing volume or audio queue timing. */
bool gpsp_audio_muted(void);
void gpsp_audio_set_muted(bool muted);

/**
 * Queued sound in milliseconds, queue shortages, and I2S send queue
 * overflows (the writer fell behind the DMA; often a short silence).
 */
void gpsp_audio_stats(unsigned *queued_ms, unsigned *underruns,
        unsigned *i2s_overflows);

void gpsp_audio_shutdown(void);

#endif /* GPSP_AUDIO_H */
