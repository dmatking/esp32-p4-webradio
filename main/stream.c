// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// HTTP audio stream with Icecast ICY metadata support.
// Writes raw audio bytes into a FreeRTOS stream buffer; the MP3 decode
// task reads from the other side.

#include <string.h>
#include <stdlib.h>
#include "stream.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#define TAG "stream"

#define RING_BUF_SIZE   (64 * 1024)
#define READ_BUF_SIZE   2048
#define TASK_STACK      (16 * 1024)

// ── Ring buffer ──────────────────────────────────────────────────────
static StreamBufferHandle_t s_ringbuf;

// ── Task control ─────────────────────────────────────────────────────
static TaskHandle_t s_task;
static volatile bool s_running;
static char s_url[300];
static icy_meta_cb_t s_meta_cb;

// ── ICY metadata state ───────────────────────────────────────────────
static int  s_icy_metaint;   // 0 = no ICY metadata in this stream
static int  s_icy_count;     // bytes until next metadata block

// Parse "StreamTitle='...'" from an ICY metadata block.
static void parse_icy_meta(const char *meta, int len)
{
    const char *key = "StreamTitle='";
    const char *p = strnstr(meta, key, len);
    if (!p) return;
    p += strlen(key);
    const char *end = memchr(p, '\'', meta + len - p);
    if (!end || end == p) return;

    int tlen = end - p;
    char title[256];
    if (tlen >= (int)sizeof(title)) tlen = sizeof(title) - 1;
    memcpy(title, p, tlen);
    title[tlen] = '\0';

    ESP_LOGI(TAG, "ICY: %s", title);
    if (s_meta_cb) s_meta_cb(title);
}

// Feed raw bytes into the ring buffer, handling ICY metadata boundaries.
// `data` is raw HTTP body bytes (interleaved audio + ICY blocks when enabled).
static void feed_data(const uint8_t *data, int len)
{
    while (len > 0) {
        if (s_icy_metaint > 0 && s_icy_count == 0) {
            // Next byte is the ICY metadata length prefix
            int meta_len = (*data) * 16;
            data++; len--;
            if (meta_len > 0 && meta_len <= len) {
                parse_icy_meta((const char *)data, meta_len);
                data += meta_len;
                len  -= meta_len;
            } else if (meta_len > 0) {
                // Metadata spans across HTTP chunks — skip it
                data += len;
                len = 0;
            }
            s_icy_count = s_icy_metaint;
            continue;
        }

        // How many audio bytes until next metadata boundary?
        int chunk = len;
        if (s_icy_metaint > 0 && chunk > s_icy_count)
            chunk = s_icy_count;

        // Write audio bytes to ring buffer (block up to 500 ms)
        size_t written = xStreamBufferSend(s_ringbuf, data, chunk,
                                            pdMS_TO_TICKS(500));
        data += written;
        len  -= written;
        if (s_icy_metaint > 0) s_icy_count -= written;

        if (written == 0) {
            // Buffer full and timed out — drop remaining chunk
            ESP_LOGW(TAG, "Ring buffer full, dropping %d bytes", len);
            break;
        }
    }
}

// ── HTTP event handler ───────────────────────────────────────────────
static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            s_icy_metaint = atoi(evt->header_value);
            s_icy_count   = s_icy_metaint;
            ESP_LOGI(TAG, "ICY metaint = %d", s_icy_metaint);
        }
    }
    return ESP_OK;
}

// ── Streaming task ───────────────────────────────────────────────────
static void stream_task(void *arg)
{
    ESP_LOGI(TAG, "Connecting to %s", s_url);

    esp_http_client_config_t cfg = {
        .url            = s_url,
        .event_handler  = http_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms     = 10000,
        .buffer_size    = READ_BUF_SIZE,
        .user_agent     = "p4-radio/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    // Request ICY metadata
    esp_http_client_set_header(client, "Icy-MetaData", "1");

    // Open with redirect handling (esp_http_client_open doesn't auto-follow)
    int max_redirects = 5;
    int status = 0;
    while (max_redirects-- > 0) {
        s_icy_metaint = 0;
        s_icy_count   = 0;

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            goto done;
        }

        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP %d", status);

        if (status == 301 || status == 302 || status == 307 || status == 308) {
            err = esp_http_client_set_redirection(client);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Redirect failed");
                goto done;
            }
            ESP_LOGI(TAG, "Following redirect...");
            // Re-set ICY header (lost after redirect)
            esp_http_client_set_header(client, "Icy-MetaData", "1");
            continue;
        }
        break;
    }

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP error %d", status);
        goto done;
    }

    // Read loop
    uint8_t *buf = malloc(READ_BUF_SIZE);
    if (!buf) goto done;

    while (s_running) {
        int n = esp_http_client_read(client, (char *)buf, READ_BUF_SIZE);
        if (n < 0) {
            ESP_LOGE(TAG, "Read error");
            break;
        }
        if (n == 0) {
            ESP_LOGI(TAG, "Stream ended");
            break;
        }
        feed_data(buf, n);
    }

    free(buf);

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    s_running = false;
    ESP_LOGI(TAG, "Stream task exiting");
    vTaskDelete(NULL);
}

// ── Public API ───────────────────────────────────────────────────────

void stream_start(const char *url, icy_meta_cb_t meta_cb)
{
    stream_stop();

    s_icy_metaint = 0;
    s_icy_count   = 0;
    s_meta_cb     = meta_cb;
    snprintf(s_url, sizeof(s_url), "%s", url);

    if (!s_ringbuf) {
        s_ringbuf = xStreamBufferCreate(RING_BUF_SIZE, 1);
    } else {
        xStreamBufferReset(s_ringbuf);
    }

    s_running = true;
    xTaskCreate(stream_task, "stream", TASK_STACK, NULL, 5, &s_task);
}

void stream_stop(void)
{
    if (s_running) {
        s_running = false;
        // Give task time to exit
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (s_ringbuf) {
        xStreamBufferReset(s_ringbuf);
    }
    s_task = NULL;
}

int stream_read(uint8_t *buf, int len, uint32_t timeout_ms)
{
    if (!s_ringbuf) return 0;
    return xStreamBufferReceive(s_ringbuf, buf, len, pdMS_TO_TICKS(timeout_ms));
}

bool stream_is_active(void)
{
    return s_running;
}
