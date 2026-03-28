// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// ES8311 codec + I2S driver for audio output

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Initialize I2C, ES8311 codec, I2S TX channel, and PA.
// Default: 44100 Hz, 16-bit stereo.
esp_err_t audio_init(void);

// Write interleaved 16-bit stereo PCM to I2S.
esp_err_t audio_write(const int16_t *samples, size_t num_samples,
                      size_t *samples_written, uint32_t timeout_ms);

// Reconfigure sample rate (e.g. 22050, 44100, 48000).
esp_err_t audio_set_sample_rate(int rate);

// Set volume 0–100.
esp_err_t audio_set_volume(int vol);

// Enable/disable power amplifier.
void audio_pa_enable(bool on);

// Play a 440 Hz test tone for the given duration (ms). Blocking.
void audio_test_tone(int duration_ms);
