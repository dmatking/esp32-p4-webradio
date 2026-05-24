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
#include "esp_heap_caps.h"
#include "nvs.h"
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

// SoftAP name shown while the WiFi provisioning portal is active
static char s_ap_ssid[33] = "";

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
    STATE_PROVISIONING,         // pulsing teal — captive-portal setup AP is up
    STATE_FETCHING_STATIONS,    // pulsing yellow
    STATE_PLAYING,              // spectrum visualizer
    STATE_WIFI_FAILED,          // pulsing red — could not join WiFi
    STATE_API_FAILED,           // pulsing orange — joined WiFi, station API failed
    STATE_UNAVAILABLE,          // station refused the stream (e.g. HTTP 403)
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

// ── Persisted settings (NVS) ─────────────────────────────────────────
// NVS is already initialized by the wifi_prov component during connect.
#define SETTINGS_NS "radio"

static void settings_save_volume(int vol)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "vol", (uint8_t)vol);
    nvs_commit(h);
    nvs_close(h);
}

static void settings_save_station(const char *url)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "url", url);
    nvs_commit(h);
    nvs_close(h);
}

// Load the saved volume into s_volume (leaves the default if none stored).
static void settings_load_volume(void)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "vol", &v) == ESP_OK && v <= 100) s_volume = v;
    nvs_close(h);
}

// Return the index of the saved station URL within the fetched list, or -1.
static int settings_saved_station_idx(void)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) return -1;
    char url[STATION_URL_LEN];
    size_t len = sizeof(url);
    int idx = -1;
    if (nvs_get_str(h, "url", url, &len) == ESP_OK) {
        for (int i = 0; i < s_station_count; i++) {
            if (strcmp(s_stations[i].url, url) == 0) { idx = i; break; }
        }
    }
    nvs_close(h);
    return idx;
}

// ── Header overlay (station name, song title, index, volume) ─────────

static void draw_header(uint8_t *buf)
{
    if (s_station_count <= 0) return;

    // Station index — small, top-left corner
    char idx_buf[24];
    snprintf(idx_buf, sizeof(idx_buf), "%d/%d", s_station_idx + 1, s_station_count);
    font_puts(buf, 8, 4, idx_buf, 120, 120, 120);

    // Volume — small, top-right corner
    char vol_buf[16];
    snprintf(vol_buf, sizeof(vol_buf), s_muted ? "MUTE" : "Vol %d", s_volume);
    int vol_w = strlen(vol_buf) * FONT_W;
    font_puts(buf, DISP_W - vol_w - 8, 4, vol_buf, 120, 120, 120);

    // Station name — large, centered, white. Shrink a scale if it would
    // overflow the screen width.
    const char *name = s_stations[s_station_idx].name;
    int name_scale = 3;
    if (font_text_w(name, name_scale) > DISP_W - 8) name_scale = 2;
    font_puts_center(buf, 8, name, name_scale, 255, 255, 255);

    // Song title — same size as station name, amber to differentiate.
    if (s_song_title[0]) {
        int title_scale = 3;
        if (font_text_w(s_song_title, title_scale) > DISP_W - 8) title_scale = 2;
        if (font_text_w(s_song_title, title_scale) > DISP_W - 8) title_scale = 1;
        font_puts_center(buf, 8 + name_scale * FONT_H + 6, s_song_title,
                         title_scale, 0, 200, 255);  // amber (BGR)
    }
}

// ── Spectrum bar rendering ───────────────────────────────────────────

static void draw_spectrum(uint8_t *buf)
{
    const float *bands = spectrum_get_bands();

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
    }

    // Text overlay in the top area.
    draw_header(buf);
}

// ── Frame dispatch ───────────────────────────────────────────────────

static void draw_frame(int frame)
{
    (void)frame;
    uint8_t *buf = display_backbuf();

    // Status screens: solid black background, large centered text.
    if (s_app_state != STATE_PLAYING) {
        memset(buf, 0, DISP_FB_SIZE);
    }

    switch (s_app_state) {
    case STATE_WIFI_CONNECTING:
        font_puts_center(buf, 320, "Connecting...", 4, 255, 255, 0);  // cyan
        break;
    case STATE_PROVISIONING:
        font_puts_center(buf, 110, "WiFi Setup", 4, 255, 255, 255);
        font_puts_center(buf, 250, "1. Join WiFi network:", 2, 255, 255, 255);
        font_puts_center(buf, 300, s_ap_ssid, 3, 0, 255, 255);        // yellow
        font_puts_center(buf, 410, "2. Open in browser:", 2, 255, 255, 255);
        font_puts_center(buf, 460, "192.168.4.1", 3, 255, 255, 0);    // cyan
        break;
    case STATE_FETCHING_STATIONS:
        font_puts_center(buf, 320, "Loading Stations", 4, 0, 220, 255);  // yellow
        break;
    case STATE_WIFI_FAILED:
        font_puts_center(buf, 230, "WiFi Failed", 4, 0, 0, 255);         // red
        font_puts_center(buf, 380, "Check network and reboot", 2, 255, 255, 255);
        break;
    case STATE_API_FAILED:
        font_puts_center(buf, 230, "Station API Error", 4, 0, 140, 255); // orange
        font_puts_center(buf, 360, "Retrying...", 3, 255, 255, 255);
        break;
    case STATE_UNAVAILABLE:
        draw_header(buf);  // keep station name/index so the user knows which
        font_puts_center(buf, 330, "Currently", 4, 255, 255, 255);
        font_puts_center(buf, 410, "Unavailable", 4, 0, 140, 255);  // orange
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

// Captive-portal provisioning has started — show setup instructions.
static void on_wifi_portal(const char *ap_ssid)
{
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", ap_ssid);
    s_app_state = STATE_PROVISIONING;
    ESP_LOGI(TAG, "Provisioning portal up — join '%s', open 192.168.4.1", ap_ssid);
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
    settings_save_station(st->url);
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

    if (!wifi_connect(on_wifi_portal)) {
        s_app_state = STATE_WIFI_FAILED;
        vTaskDelete(NULL);
        return;
    }

    // Fetch the station list, retrying with backoff — the Radio Browser API
    // is flaky and a single miss shouldn't strand the radio. Keep trying so
    // it self-heals once the API comes back.
    s_app_state = STATE_FETCHING_STATIONS;
    int backoff_ms = 3000;
    for (int attempt = 1; ; attempt++) {
        s_station_count = radio_browser_fetch(s_stations, MAX_STATIONS);
        if (s_station_count > 0) break;

        ESP_LOGW(TAG, "Station fetch failed (attempt %d), retrying in %d ms",
                 attempt, backoff_ms);
        s_app_state = STATE_API_FAILED;
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        if (backoff_ms < 15000) backoff_ms += 3000;  // cap at 15 s
        s_app_state = STATE_FETCHING_STATIONS;
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

    // Restore the saved volume (falls back to the default if none stored).
    settings_load_volume();
    audio_set_volume(s_volume);

    // Initialize touch (GT911 on same I2C bus)
    if (touch_init() == ESP_OK) {
        s_touch_ready = true;
        s_touch_q = xQueueCreate(8, sizeof(touch_event_t));
        xTaskCreate(touch_task, "touch", 4096, NULL, 6, NULL);
    } else {
        ESP_LOGW(TAG, "Touch init failed — continuing without touch");
    }

    // Resume the last station if it's still in the list; otherwise fall back
    // to a known-working Texas music station.
    int start_idx = settings_saved_station_idx();
    if (start_idx < 0) {
        start_idx = 0;
        const char *prefs[] = { "Radio Free Texas", "KUTX", "KMFA", "KERA", NULL };
        for (const char **p = prefs; *p; p++) {
            for (int i = 0; i < s_station_count; i++) {
                if (strcasestr(s_stations[i].name, *p)) {
                    start_idx = i;
                    goto found;
                }
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

    while (1) {
        draw_frame(frame++);
        display_flush();

        // Handle touch input — drain everything the touch task has queued.
        touch_event_t ev;
        while ((s_app_state == STATE_PLAYING || s_app_state == STATE_UNAVAILABLE) &&
               s_touch_q && xQueueReceive(s_touch_q, &ev, 0)) {
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
                s_volume += 2;
                if (s_volume > 100) s_volume = 100;
                audio_set_volume(s_volume);
                if (s_muted) { s_muted = false; audio_pa_enable(true); }
                settings_save_volume(s_volume);
                ESP_LOGI(TAG, "Touch: volume up → %d", s_volume);
                break;
            case TOUCH_EVENT_SWIPE_DOWN:
                s_volume -= 2;
                if (s_volume < 0) s_volume = 0;
                audio_set_volume(s_volume);
                settings_save_volume(s_volume);
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

        // React to a stream that isn't feeding audio. Suppressed while a
        // selection is pending (the stream is intentionally about to change).
        if (s_pending_idx < 0 && !stream_is_active() &&
            (s_app_state == STATE_PLAYING || s_app_state == STATE_UNAVAILABLE)) {
            if (stream_failed()) {
                // The station refused us (e.g. HTTP 403). Show "Currently
                // Unavailable" and stay put — the user can page elsewhere.
                if (s_app_state != STATE_UNAVAILABLE) {
                    ESP_LOGW(TAG, "Stream refused, marking station unavailable");
                    s_app_state = STATE_UNAVAILABLE;
                }
                dead_since = 0;
            } else {
                // Connected then died — auto-advance after ~5 s.
                TickType_t now = xTaskGetTickCount();
                if (dead_since == 0) dead_since = now;
                if ((now - dead_since) >= pdMS_TO_TICKS(5000)) {
                    ESP_LOGW(TAG, "Stream dead, advancing to next station");
                    int next = (s_station_idx + 1) % s_station_count;
                    commit_station(next);
                    dead_since = 0;
                }
            }
        } else {
            dead_since = 0;
        }
    }
}
