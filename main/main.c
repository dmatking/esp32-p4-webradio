// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// p4-radio: Internet radio for Waveshare ESP32-P4 720x720
// Phase 6: UI + Touch control

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "display.h"
#include "wifi.h"
#include "radio_browser.h"
#include "audio.h"
#include "stream.h"
#include "mp3_decoder.h"
#include "spectrum.h"
#include "font8x16.h"
#include "touch.h"

#define TAG "main"

// Station list stored in PSRAM
static radio_station_t *s_stations;
static int s_station_count = 0;
static int s_station_idx   = 0;

// Current song title from ICY metadata
static char s_song_title[256] = "";

// Volume (0–100) and mute state
static int  s_volume = 60;
static bool s_muted  = false;
static bool s_touch_ready = false;

// Touch runs in its own task so gestures are sampled densely (every
// TOUCH_POLL_MS) regardless of how long a render frame takes. Events are
// handed to the main loop via a small queue.
#define TOUCH_POLL_MS  15
static QueueHandle_t s_touch_q;

static void touch_task(void *arg)
{
    for (;;) {
        touch_event_t ev = touch_poll();
        if (ev != TOUCH_EVENT_NONE) {
            xQueueSend(s_touch_q, &ev, 0);  // drop if full, never block
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}

// App state
typedef enum {
    STATE_WIFI_CONNECTING = 0,  // pulsing blue
    STATE_FETCHING_STATIONS,    // pulsing yellow
    STATE_PLAYING,              // spectrum visualizer
    STATE_WIFI_FAILED,          // solid red
} app_state_t;
static volatile app_state_t s_app_state = STATE_WIFI_CONNECTING;

// ── Color utilities ──────────────────────────────────────────────────

// Hue (0–359) → BGR pixel
static void hue_to_bgr(int hue, uint8_t *b, uint8_t *g, uint8_t *r)
{
    int h = hue / 60;
    int f = (hue % 60) * 255 / 60;
    int q = 255 - f;
    uint8_t rv, gv, bv;
    switch (h) {
        case 0: rv=255; gv=f;   bv=0;   break;
        case 1: rv=q;   gv=255; bv=0;   break;
        case 2: rv=0;   gv=255; bv=f;   break;
        case 3: rv=0;   gv=q;   bv=255; break;
        case 4: rv=f;   gv=0;   bv=255; break;
        default:rv=255; gv=0;   bv=q;   break;
    }
    *b = bv; *g = gv; *r = rv;
}

// Solid-color pulse fill (for status states)
static void fill_pulse(uint8_t *buf, int frame, uint8_t br, uint8_t bg, uint8_t bb)
{
    int t = frame % 60;
    int bright = (t < 30 ? t : 60 - t) * 8;
    if (bright > 220) bright = 220;
    uint8_t bv = (uint8_t)(bb * bright / 220);
    uint8_t gv = (uint8_t)(bg * bright / 220);
    uint8_t rv = (uint8_t)(br * bright / 220);
    uint16_t px = disp_rgb565(rv, gv, bv);
    uint16_t *p = (uint16_t *)buf;
    for (int i = 0; i < DISP_W * DISP_H; i++) p[i] = px;
}

// ── Spectrum bar rendering ───────────────────────────────────────────

static void draw_spectrum(uint8_t *buf)
{
    const float *bands = spectrum_get_bands();
    const float *peaks = spectrum_get_peaks();

    // Black background
    memset(buf, 0, DISP_W * DISP_H * DISP_BPP);

    const int bar_w    = 20;
    const int gap      = 2;
    const int stride   = bar_w + gap;
    const int margin_x = (DISP_W - stride * SPECTRUM_BANDS + gap) / 2;
    const int bar_area_top = DISP_H * 15 / 100;
    const int bar_area_h   = DISP_H - bar_area_top;

    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        float mag = bands[b];
        if (mag < 0) mag = 0;
        if (mag > 1.0f) mag = 1.0f;

        int bar_h = (int)(mag * bar_area_h);
        int x0 = margin_x + b * stride;

        // Color: hue from blue (240) at bass to red (0) at treble
        int hue = 240 - (b * 240 / SPECTRUM_BANDS);
        if (hue < 0) hue += 360;
        uint8_t cb, cg, cr;
        hue_to_bgr(hue, &cb, &cg, &cr);

        // Draw bar from bottom up
        for (int dy = 0; dy < bar_h; dy++) {
            int y = DISP_H - 1 - dy;
            if (y < bar_area_top) break;

            int bright = 140 + (dy * 115 / (bar_area_h > 0 ? bar_area_h : 1));
            if (bright > 255) bright = 255;

            uint8_t pb = (uint8_t)(cb * bright / 255);
            uint8_t pg = (uint8_t)(cg * bright / 255);
            uint8_t pr = (uint8_t)(cr * bright / 255);

            uint16_t px565 = disp_rgb565(pr, pg, pb);
            uint16_t *row = (uint16_t *)buf + y * DISP_W;
            for (int dx = 0; dx < bar_w && (x0 + dx) < DISP_W; dx++) {
                row[x0 + dx] = px565;
            }
        }

        // Peak dot: 3px white marker at the peak position (falls slower)
        float pk = peaks[b];
        if (pk < 0) pk = 0;
        if (pk > 1.0f) pk = 1.0f;
        int peak_h = (int)(pk * bar_area_h);
        if (peak_h > 3) {
            int cap_y = DISP_H - peak_h;
            if (cap_y >= bar_area_top && cap_y < DISP_H) {
                for (int cy = 0; cy < 3 && (cap_y + cy) < DISP_H; cy++) {
                    uint16_t *row = (uint16_t *)buf + (cap_y + cy) * DISP_W;
                    for (int dx = 0; dx < bar_w && (x0 + dx) < DISP_W; dx++) {
                        row[x0 + dx] = 0xFFFF;  // white
                    }
                }
            }
        }
    }

    // ── Text overlay in top area ─────────────────────────────────────
    if (s_station_count > 0) {
        // Station name — 2x scale (16x32), centered, white
        const char *name = s_stations[s_station_idx].name;
        int name_len = strlen(name);
        int name_w = name_len * FONT_W * 2;
        int name_x = (DISP_W - name_w) / 2;
        if (name_x < 4) name_x = 4;
        font_puts_2x(buf, name_x, 8, name, 255, 255, 255);

        // Song title — 1x scale (8x16), centered, light cyan
        if (s_song_title[0]) {
            int title_len = strlen(s_song_title);
            int title_w = title_len * FONT_W;
            int title_x = (DISP_W - title_w) / 2;
            if (title_x < 4) title_x = 4;
            font_puts(buf, title_x, 44, s_song_title, 200, 220, 180);
        }

        // Station index — small, bottom-left
        char idx_buf[16];
        snprintf(idx_buf, sizeof(idx_buf), "%d/%d", s_station_idx + 1, s_station_count);
        font_puts(buf, 8, 68, idx_buf, 100, 100, 100);

        // Volume — small, bottom-right of header area
        char vol_buf[16];
        snprintf(vol_buf, sizeof(vol_buf), s_muted ? "MUTE" : "Vol %d", s_volume);
        int vol_w = strlen(vol_buf) * FONT_W;
        font_puts(buf, DISP_W - vol_w - 8, 68, vol_buf, 100, 100, 100);
    }

}

// ── Frame dispatch ───────────────────────────────────────────────────

static void draw_frame(int frame)
{
    uint8_t *buf = display_backbuf();
    switch (s_app_state) {
    case STATE_WIFI_CONNECTING:
        fill_pulse(buf, frame, 0, 0, 255);
        font_puts_2x(buf, 200, 340, "Connecting...", 255, 255, 255);
        break;
    case STATE_FETCHING_STATIONS:
        fill_pulse(buf, frame, 220, 180, 0);
        font_puts_2x(buf, 160, 340, "Loading stations", 255, 255, 255);
        break;
    case STATE_WIFI_FAILED:
        fill_pulse(buf, frame, 255, 0, 0);
        font_puts_2x(buf, 240, 340, "WiFi failed", 255, 255, 255);
        break;
    case STATE_PLAYING:
        draw_spectrum(buf);
        break;
    }
}

// ── Callbacks ────────────────────────────────────────────────────────

static void on_song_title(const char *title)
{
    snprintf(s_song_title, sizeof(s_song_title), "%s", title);
    ESP_LOGI(TAG, "Now playing: %s", s_song_title);
}

// ── Station control ──────────────────────────────────────────────────

// Debounced station selection: a tap moves the highlight and updates the
// UI immediately, but we hold off tearing down / starting a stream until the
// user has stopped paging for STATION_DEBOUNCE_MS.
#define STATION_DEBOUNCE_MS 300

static int        s_pending_idx  = -1;   // -1 = nothing pending
static TickType_t s_pending_tick = 0;

// Actually switch the stream to `idx`. Heavy: stops/starts tasks.
static void commit_station(int idx)
{
    if (idx < 0 || idx >= s_station_count) return;

    stream_stop();
    mp3dec_stop();
    vTaskDelay(pdMS_TO_TICKS(200));  // let tasks clean up

    s_station_idx = idx;
    radio_station_t *st = &s_stations[idx];
    ESP_LOGI(TAG, "Tuning to [%d]: %s (%u kbps)", idx, st->name, st->bitrate);
    ESP_LOGI(TAG, "  URL: %s", st->url);

    s_song_title[0] = '\0';
    stream_start(st->url, on_song_title);
    mp3dec_start(NULL);
    s_app_state = STATE_PLAYING;
}

// Touch-driven selection: update highlight now, defer the stream start.
static void select_station(int idx)
{
    if (idx < 0 || idx >= s_station_count) return;
    s_station_idx  = idx;        // UI reflects selection immediately
    s_song_title[0] = '\0';
    s_pending_idx  = idx;
    s_pending_tick = xTaskGetTickCount();
    s_app_state    = STATE_PLAYING;
}

// ── Network task ─────────────────────────────────────────────────────

static void network_task(void *arg)
{
    s_stations = heap_caps_malloc(sizeof(radio_station_t) * MAX_STATIONS,
                                  MALLOC_CAP_SPIRAM);
    if (!s_stations) {
        ESP_LOGE(TAG, "Failed to alloc station list");
        s_app_state = STATE_WIFI_FAILED;
        vTaskDelete(NULL);
        return;
    }

    if (!wifi_connect()) {
        s_app_state = STATE_WIFI_FAILED;
        vTaskDelete(NULL);
        return;
    }

    s_app_state = STATE_FETCHING_STATIONS;
    s_station_count = radio_browser_fetch(s_stations, MAX_STATIONS);
    if (s_station_count <= 0) {
        ESP_LOGE(TAG, "No stations fetched");
        s_app_state = STATE_WIFI_FAILED;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Fetched %d stations", s_station_count);

    // Initialize audio + spectrum
    if (audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "Audio init failed");
        s_app_state = STATE_WIFI_FAILED;
        vTaskDelete(NULL);
        return;
    }
    spectrum_init();

    // Initialize touch (GT911 on same I2C bus)
    if (touch_init() == ESP_OK) {
        s_touch_ready = true;
        s_touch_q = xQueueCreate(8, sizeof(touch_event_t));
        xTaskCreate(touch_task, "touch", 4096, NULL, 6, NULL);
    } else {
        ESP_LOGW(TAG, "Touch init failed — continuing without touch");
    }

    // Prefer a known-working Texas music station
    int start_idx = 0;
    const char *prefs[] = { "Radio Free Texas", "KUTX", "KMFA", "KERA", NULL };
    for (const char **p = prefs; *p; p++) {
        for (int i = 0; i < s_station_count; i++) {
            if (strcasestr(s_stations[i].name, *p)) {
                start_idx = i;
                goto found;
            }
        }
    }
found:
    commit_station(start_idx);
    vTaskDelete(NULL);
}

// ── Main ─────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "p4-radio Phase 6: UI + Touch");
    display_init();

    xTaskCreate(network_task, "network", 32768, NULL, 5, NULL);

    int frame = 0;
    TickType_t dead_since = 0;   // tick when stream first looked dead (0 = alive)
    // Temporary frame-time instrumentation (remove after perf tuning).
    int64_t fps_accum = 0;
    int     fps_n = 0;

    while (1) {
        int64_t t0 = esp_timer_get_time();
        draw_frame(frame++);
        display_flush();
        fps_accum += esp_timer_get_time() - t0;
        if (++fps_n >= 120) {
            int us = (int)(fps_accum / fps_n);
            ESP_LOGI(TAG, "render: %d us/frame (%d fps)", us, us > 0 ? 1000000 / us : 0);
            fps_accum = 0;
            fps_n = 0;
        }

        // Handle touch input — drain everything the touch task has queued.
        touch_event_t ev;
        while (s_app_state == STATE_PLAYING && s_touch_q &&
               xQueueReceive(s_touch_q, &ev, 0)) {
            switch (ev) {
            case TOUCH_EVENT_TAP_LEFT:
                ESP_LOGI(TAG, "Touch: previous station");
                {
                    int prev = (s_station_idx - 1 + s_station_count) % s_station_count;
                    select_station(prev);
                    dead_since = 0;
                }
                break;
            case TOUCH_EVENT_TAP_RIGHT:
                ESP_LOGI(TAG, "Touch: next station");
                {
                    int next = (s_station_idx + 1) % s_station_count;
                    select_station(next);
                    dead_since = 0;
                }
                break;
            case TOUCH_EVENT_TAP_CENTER:
                s_muted = !s_muted;
                audio_pa_enable(!s_muted);
                ESP_LOGI(TAG, "Touch: %s", s_muted ? "muted" : "unmuted");
                break;
            case TOUCH_EVENT_SWIPE_UP:
                s_volume += 10;
                if (s_volume > 100) s_volume = 100;
                audio_set_volume(s_volume);
                if (s_muted) { s_muted = false; audio_pa_enable(true); }
                ESP_LOGI(TAG, "Touch: volume up → %d", s_volume);
                break;
            case TOUCH_EVENT_SWIPE_DOWN:
                s_volume -= 10;
                if (s_volume < 0) s_volume = 0;
                audio_set_volume(s_volume);
                ESP_LOGI(TAG, "Touch: volume down → %d", s_volume);
                break;
            default:
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));

        // Commit a pending (debounced) station selection once the user has
        // stopped paging for STATION_DEBOUNCE_MS.
        if (s_pending_idx >= 0 &&
            (xTaskGetTickCount() - s_pending_tick) >= pdMS_TO_TICKS(STATION_DEBOUNCE_MS)) {
            int idx = s_pending_idx;
            s_pending_idx = -1;
            commit_station(idx);
            dead_since = 0;
            continue;
        }

        // Auto-advance: if the stream stays dead for ~5 s, try the next
        // station. Suppressed while a selection is pending (the stream is
        // intentionally about to change).
        if (s_app_state == STATE_PLAYING && s_pending_idx < 0 && !stream_is_active()) {
            TickType_t now = xTaskGetTickCount();
            if (dead_since == 0) dead_since = now;
            if ((now - dead_since) >= pdMS_TO_TICKS(5000)) {
                ESP_LOGW(TAG, "Stream dead, advancing to next station");
                int next = (s_station_idx + 1) % s_station_count;
                commit_station(next);
                dead_since = 0;
            }
        } else {
            dead_since = 0;
        }
    }
}
