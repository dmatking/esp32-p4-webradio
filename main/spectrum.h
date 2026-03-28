// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Spectrum analyzer — FFT on PCM samples, outputs 32 log-scale bands

#pragma once

#include <stdint.h>

#define SPECTRUM_BANDS  32
#define SPECTRUM_FFT_N  512

// Initialize FFT tables and window. Call once at startup.
void spectrum_init(void);

// Feed interleaved stereo 16-bit PCM samples. Thread-safe.
// Accumulates into an internal buffer; runs FFT when full.
void spectrum_feed(const int16_t *samples, int num_stereo_samples);

// Get current band magnitudes (0.0–1.0 range, with decay applied).
// Returns pointer to SPECTRUM_BANDS floats. Valid until next call.
const float *spectrum_get_bands(void);

// Get current peak magnitudes (fall slower than bands).
const float *spectrum_get_peaks(void);
