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
#include "esp_log.h"
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
    for (int i = 0; i < DISP_W * DISP_H; i++) {
        buf[i * DISP_BPP + 0] = bv;
        buf[i * DISP_BPP + 1] = gv;
        buf[i * DISP_BPP + 2] = rv;
    }
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

            uint8_t *row = buf + y * DISP_W * DISP_BPP;
            for (int dx = 0; dx < bar_w && (x0 + dx) < DISP_W; dx++) {
                int px = (x0 + dx) * DISP_BPP;
                row[px + 0] = pb;
                row[px + 1] = pg;
                row[px + 2] = pr;
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
                    uint8_t *row = buf + (cap_y + cy) * DISP_W * DISP_BPP;
                    for (int dx = 0; dx < bar_w && (x0 + dx) < DISP_W; dx++) {
                        int px = (x0 + dx) * DISP_BPP;
                        row[px + 0] = 255;
                        row[px + 1] = 255;
                        row[px + 2] = 255;
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

    // Touch debug (temporary)
    font_puts(buf, 8, 88, touch_debug_str(), 255, 255, 0);
    uint16_t tx, ty, rx, ry;
    if (touch_debug_pos(&tx, &ty)) {
        char pos[48];
        touch_debug_raw(&rx, &ry);
        snprintf(pos, sizeof(pos), "S:%d,%d R:%d,%d", tx, ty, rx, ry);
        font_puts(buf, 8, DISP_H - 20, pos, 0, 255, 0);
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

static TickType_t s_last_switch_tick = 0;

static void play_station(int idx)
{
    if (idx < 0 || idx >= s_station_count) return;

    // Prevent rapid switching (min 2 seconds between switches)
    TickType_t now = xTaskGetTickCount();
    if ((now - s_last_switch_tick) < pdMS_TO_TICKS(2000) && s_last_switch_tick != 0) {
        ESP_LOGW(TAG, "Station switch too fast, ignoring");
        return;
    }
    s_last_switch_tick = now;

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
    play_station(start_idx);
    vTaskDelete(NULL);
}

// ── Main ─────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "p4-radio Phase 6: UI + Touch");
    display_init();

    xTaskCreate(network_task, "network", 32768, NULL, 5, NULL);

    int frame = 0;
    int no_audio_frames = 0;
    while (1) {
        draw_frame(frame++);
        display_flush();

        // Handle touch input (poll every 4th frame ~130ms to reduce I2C pressure)
        if (s_touch_ready && s_app_state == STATE_PLAYING && (frame % 2 == 0)) {
            touch_event_t ev = touch_poll();
            switch (ev) {
            case TOUCH_EVENT_TAP_LEFT:
                ESP_LOGI(TAG, "Touch: previous station");
                {
                    int prev = (s_station_idx - 1 + s_station_count) % s_station_count;
                    play_station(prev);
                    no_audio_frames = 0;
                }
                break;
            case TOUCH_EVENT_TAP_RIGHT:
                ESP_LOGI(TAG, "Touch: next station");
                {
                    int next = (s_station_idx + 1) % s_station_count;
                    play_station(next);
                    no_audio_frames = 0;
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

        vTaskDelay(pdMS_TO_TICKS(33));

        // Auto-advance: if stream dies, try the next station
        if (s_app_state == STATE_PLAYING && !stream_is_active()) {
            no_audio_frames++;
            if (no_audio_frames > 150) {
                ESP_LOGW(TAG, "Stream dead, advancing to next station");
                int next = (s_station_idx + 1) % s_station_count;
                play_station(next);
                no_audio_frames = 0;
            }
        } else {
            no_audio_frames = 0;
        }
    }
}
