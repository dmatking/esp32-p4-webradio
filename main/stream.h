// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// HTTP audio stream with ring buffer and ICY metadata parsing

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ICY metadata callback — called from stream task when StreamTitle changes.
typedef void (*icy_meta_cb_t)(const char *stream_title);

// Start streaming from the given URL. Spawns an HTTP reader task.
void stream_start(const char *url, icy_meta_cb_t meta_cb);

// Stop the streaming task and drain the buffer.
void stream_stop(void);

// Read audio bytes from the ring buffer (blocks up to timeout_ms).
// Returns number of bytes read (0 on timeout / stream ended).
int stream_read(uint8_t *buf, int len, uint32_t timeout_ms);

// True if the streaming task is alive and feeding data.
bool stream_is_active(void);
