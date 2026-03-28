// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// MP3 decode task — reads from stream ring buffer, writes PCM to I2S

#pragma once

#include <stdbool.h>

// Callback when ICY StreamTitle changes (forwarded from stream layer).
typedef void (*title_cb_t)(const char *title);

// Start the MP3 decode task. Call after audio_init() and stream_start().
void mp3dec_start(title_cb_t cb);

// Stop the decode task.
void mp3dec_stop(void);

// True while the decode task is running.
bool mp3dec_is_running(void);
