/* Board output for the P4 Nano audio check.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIO_TEST_OUTPUT_H
#define AUDIO_TEST_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t audio_test_output_init(void);
esp_err_t audio_test_output_volume(int db);
esp_err_t audio_test_output_mute(bool mute);
esp_err_t audio_test_output_amplifier(bool enable);
esp_err_t audio_test_output_write(const int16_t *mono, size_t frames);
void audio_test_output_stop(void);

#endif /* AUDIO_TEST_OUTPUT_H */
