// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Board-independent touch gesture layer. Reads raw points from the active
// board (board_touch_read) and turns a press/release sequence into taps
// (left/center/right thirds) and vertical swipes. The board file owns the
// touch controller hardware.

#include "touch.h"
#include "board.h"
#include "display.h"
#include "esp_log.h"
#include <stdio.h>

#define TAG "touch"

#define SWIPE_MIN_DY     80
#define SWIPE_MAX_DX     60
#define TAP_MAX_MOVE     20
#define DEBOUNCE_FRAMES  4
#define RELEASE_THRESHOLD 2  // consecutive no-touch reads before a release fires

static bool     s_ready;
static bool     s_was_touching;
static uint16_t s_start_x, s_start_y, s_last_x, s_last_y;
static int      s_debounce;
static int      s_no_touch_count;

static char     s_touch_debug[64] = "no init";
static uint16_t s_debug_x, s_debug_y;

const char *touch_debug_str(void) { return s_touch_debug; }
bool touch_debug_pos(uint16_t *x, uint16_t *y)
{
    *x = s_debug_x; *y = s_debug_y;
    return (s_debug_x != 0 || s_debug_y != 0);
}
bool touch_debug_raw(uint16_t *x, uint16_t *y)
{
    *x = s_debug_x; *y = s_debug_y;
    return (s_debug_x != 0 || s_debug_y != 0);
}

esp_err_t touch_init(void)
{
    esp_err_t r = board_touch_init();
    s_ready = (r == ESP_OK);
    snprintf(s_touch_debug, sizeof(s_touch_debug),
             s_ready ? "touch ready" : "touch not found");
    return r;
}

touch_event_t touch_poll(void)
{
    if (!s_ready) return TOUCH_EVENT_NONE;

    if (s_debounce > 0) { s_debounce--; return TOUCH_EVENT_NONE; }

    uint16_t x, y;
    bool down = board_touch_read(&x, &y);

    if (down) {
        s_no_touch_count = 0;
        s_debug_x = x; s_debug_y = y;
        if (!s_was_touching) {
            s_start_x = x; s_start_y = y;
            s_was_touching = true;
        }
        s_last_x = x; s_last_y = y;
        return TOUCH_EVENT_NONE;
    }

    if (!s_was_touching) return TOUCH_EVENT_NONE;

    // Require several consecutive no-touch reads before triggering release.
    s_no_touch_count++;
    if (s_no_touch_count < RELEASE_THRESHOLD) return TOUCH_EVENT_NONE;

    s_was_touching = false;
    s_no_touch_count = 0;

    int dx = (int)s_last_x - (int)s_start_x;
    int dy = (int)s_last_y - (int)s_start_y;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;

    touch_event_t ev = TOUCH_EVENT_NONE;
    if (ady >= SWIPE_MIN_DY && adx < SWIPE_MAX_DX) {
        ev = dy < 0 ? TOUCH_EVENT_SWIPE_UP : TOUCH_EVENT_SWIPE_DOWN;
    } else if (adx < TAP_MAX_MOVE && ady < TAP_MAX_MOVE) {
        int third = DISP_W / 3;
        if (s_start_x < third)        ev = TOUCH_EVENT_TAP_LEFT;
        else if (s_start_x > third*2) ev = TOUCH_EVENT_TAP_RIGHT;
        else                          ev = TOUCH_EVENT_TAP_CENTER;
    }

    if (ev != TOUCH_EVENT_NONE) {
        s_debounce = DEBOUNCE_FRAMES;
        ESP_LOGI(TAG, "Touch: %d (%d,%d)->(%d,%d)",
                 ev, s_start_x, s_start_y, s_last_x, s_last_y);
    }
    return ev;
}
