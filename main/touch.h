// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// GT911 capacitive touch controller on shared I2C bus

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    TOUCH_EVENT_NONE = 0,
    TOUCH_EVENT_TAP_LEFT,    // left third of screen
    TOUCH_EVENT_TAP_CENTER,  // center third
    TOUCH_EVENT_TAP_RIGHT,   // right third
    TOUCH_EVENT_SWIPE_UP,
    TOUCH_EVENT_SWIPE_DOWN,
} touch_event_t;

// Initialize GT911 on I2C bus (must call audio_init first for I2C).
esp_err_t touch_init(void);

// Poll for touch events. Call from the display loop (~30 Hz).
// Returns the most recent gesture since last call.
touch_event_t touch_poll(void);

// Debug: get init status string and current touch position
const char *touch_debug_str(void);
bool touch_debug_pos(uint16_t *x, uint16_t *y);
bool touch_debug_raw(uint16_t *x, uint16_t *y);
