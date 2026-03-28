// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// MP3 decode task — libhelix-mp3 decoder, feeds PCM to I2S via audio.c

#include <string.h>
#include "mp3_decoder.h"
#include "stream.h"
#include "audio.h"
#include "spectrum.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

// libhelix-mp3 public API
#include "mp3dec.h"

#define TAG "mp3dec"

// Input buffer: must hold at least one full MP3 frame.
// MAINBUF_SIZE (1940) is the max frame size; keep 2x for refill margin.
#define INBUF_SIZE      (MAINBUF_SIZE * 2)

// Max PCM output per frame: 2 granules * 576 samples * 2 channels = 2304 samples
#define OUTBUF_SAMPLES  (MAX_NGRAN * MAX_NSAMP * MAX_NCHAN)

#define TASK_STACK      (16 * 1024)

static TaskHandle_t s_task;
static volatile bool s_running;
static title_cb_t s_title_cb;

// Mono -> stereo expansion: duplicate each sample
static void mono_to_stereo(int16_t *buf, int mono_samples)
{
    for (int i = mono_samples - 1; i >= 0; i--) {
        buf[i * 2 + 0] = buf[i];
        buf[i * 2 + 1] = buf[i];
    }
}

static void decode_task(void *arg)
{
    HMP3Decoder decoder = MP3InitDecoder();
    if (!decoder) {
        ESP_LOGE(TAG, "Failed to create MP3 decoder");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    // Allocate buffers in PSRAM
    uint8_t *inbuf = heap_caps_malloc(INBUF_SIZE, MALLOC_CAP_SPIRAM);
    int16_t *outbuf = heap_caps_malloc(OUTBUF_SAMPLES * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM);
    if (!inbuf || !outbuf) {
        ESP_LOGE(TAG, "Buffer alloc failed");
        goto cleanup;
    }

    int bytes_in_buf = 0;
    bool rate_configured = false;

    ESP_LOGI(TAG, "Decode task started");

    while (s_running) {
        // Refill input buffer from stream
        if (bytes_in_buf < INBUF_SIZE) {
            int space = INBUF_SIZE - bytes_in_buf;
            int got = stream_read(inbuf + bytes_in_buf, space, 100);
            bytes_in_buf += got;
        }

        if (bytes_in_buf < 128) {
            // Not enough data — check if stream is still feeding us
            if (!stream_is_active()) {
                ESP_LOGI(TAG, "Stream ended, decoder stopping");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Find sync word
        int offset = MP3FindSyncWord(inbuf, bytes_in_buf);
        if (offset < 0) {
            // No sync found — discard buffer and refill
            ESP_LOGW(TAG, "No sync word in %d bytes", bytes_in_buf);
            bytes_in_buf = 0;
            continue;
        }
        if (offset > 0) {
            // Shift buffer to align sync word at start
            memmove(inbuf, inbuf + offset, bytes_in_buf - offset);
            bytes_in_buf -= offset;
        }

        // Decode one frame
        unsigned char *read_ptr = inbuf;
        int bytes_left = bytes_in_buf;
        int err = MP3Decode(decoder, &read_ptr, &bytes_left, outbuf, 0);

        if (err == ERR_MP3_INDATA_UNDERFLOW ||
            err == ERR_MP3_MAINDATA_UNDERFLOW) {
            // Need more data — keep current buffer, refill
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (err != ERR_MP3_NONE) {
            ESP_LOGW(TAG, "Decode error %d, skipping", err);
            // Skip past the bad sync word
            if (bytes_in_buf > 1) {
                memmove(inbuf, inbuf + 1, bytes_in_buf - 1);
                bytes_in_buf--;
            }
            continue;
        }

        // Consume decoded bytes from input buffer
        memmove(inbuf, read_ptr, bytes_left);
        bytes_in_buf = bytes_left;

        // Get frame info for sample rate / channels
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(decoder, &fi);

        // Configure audio output on first successful decode
        if (!rate_configured) {
            ESP_LOGI(TAG, "First frame: %d Hz, %d ch, %d kbps, layer %d",
                     fi.samprate, fi.nChans, fi.bitrate / 1000, fi.layer);
            audio_set_sample_rate(fi.samprate);
            rate_configured = true;
        }

        // Write PCM to I2S
        int pcm_samples = fi.outputSamps;  // total samples (all channels)
        if (fi.nChans == 1) {
            // Expand mono to stereo for I2S
            mono_to_stereo(outbuf, pcm_samples);
            pcm_samples *= 2;
        }

        // Feed spectrum analyzer before I2S write
        spectrum_feed(outbuf, pcm_samples);

        audio_write(outbuf, pcm_samples, NULL, 1000);
    }

cleanup:
    MP3FreeDecoder(decoder);
    heap_caps_free(inbuf);
    heap_caps_free(outbuf);
    s_running = false;
    ESP_LOGI(TAG, "Decode task exiting");
    vTaskDelete(NULL);
}

void mp3dec_start(title_cb_t cb)
{
    mp3dec_stop();
    s_title_cb = cb;
    s_running = true;
    xTaskCreate(decode_task, "mp3dec", TASK_STACK, NULL, 6, &s_task);
}

void mp3dec_stop(void)
{
    if (s_running) {
        s_running = false;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    s_task = NULL;
}

bool mp3dec_is_running(void)
{
    return s_running;
}
