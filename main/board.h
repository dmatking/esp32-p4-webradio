// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Board hardware abstraction. Each supported board provides one
// board_<name>.c implementing the display (display.h), audio (audio.h), and
// the raw touch read below. The build selects exactly one board file via
// board_config.cmake (BOARD env var), mirroring esp32-gh-dashboard.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Human-readable board name for logs.
const char *board_name(void);

// Initialize the touch controller. Called by the shared gesture layer in
// touch.c after audio_init() (the I2C bus is brought up there).
esp_err_t board_touch_init(void);

// Read one raw touch point in display coordinates. Returns true if a finger is
// currently down (and fills *x,*y), false otherwise. The shared gesture logic
// in touch.c turns a sequence of these into taps/swipes.
bool board_touch_read(uint16_t *x, uint16_t *y);
