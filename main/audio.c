// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// ES8311 codec + I2S driver — ported from vendor 06_I2SCodec example.

#include "audio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "es8311.h"
#include "esp_log.h"

#define TAG "audio"

// GPIOs — Waveshare ESP32-P4 board
#define I2C_SCL         GPIO_NUM_8
#define I2C_SDA         GPIO_NUM_7
#define I2S_MCK         GPIO_NUM_13
#define I2S_BCK         GPIO_NUM_12
#define I2S_WS          GPIO_NUM_10
#define I2S_DOUT        GPIO_NUM_9
#define I2S_DIN         GPIO_NUM_11
#define PA_EN           GPIO_NUM_53

#define I2C_PORT        I2C_NUM_0
#define I2C_CLK_HZ      100000
#define ES8311_ADDR     ES8311_ADDRRES_0   // 0x18

#define DEFAULT_RATE    44100
#define MCLK_MULTIPLE   256

static i2s_chan_handle_t s_tx_handle;
static es8311_handle_t   s_codec;
static int               s_sample_rate = DEFAULT_RATE;

esp_err_t audio_init(void)
{
    // --- PA enable (active high) ---
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << PA_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(PA_EN, 1);

    // --- I2C master (legacy driver, required by ES8311 component) ---
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_CLK_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &i2c_cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));
    ESP_LOGI(TAG, "I2C master ready");

    // --- ES8311 codec ---
    s_codec = es8311_create(I2C_PORT, ES8311_ADDR);
    if (!s_codec) {
        ESP_LOGE(TAG, "Failed to create ES8311 handle");
        return ESP_FAIL;
    }

    es8311_clock_config_t clk = {
        .mclk_inverted    = false,
        .sclk_inverted    = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency   = DEFAULT_RATE * MCLK_MULTIPLE,
        .sample_frequency = DEFAULT_RATE,
    };
    ESP_ERROR_CHECK(es8311_init(s_codec, &clk,
                                ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
    ESP_ERROR_CHECK(es8311_voice_volume_set(s_codec, 60, NULL));
    ESP_ERROR_CHECK(es8311_microphone_config(s_codec, false));
    ESP_LOGI(TAG, "ES8311 initialized at %d Hz", DEFAULT_RATE);

    // --- I2S TX channel ---
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK,
            .bclk = I2S_BCK,
            .ws   = I2S_WS,
            .dout = I2S_DOUT,
            .din  = I2S_DIN,
            .invert_flags = { false, false, false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_handle));
    ESP_LOGI(TAG, "I2S TX enabled at %d Hz stereo", DEFAULT_RATE);

    return ESP_OK;
}

esp_err_t audio_write(const int16_t *samples, size_t num_samples,
                      size_t *samples_written, uint32_t timeout_ms)
{
    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(s_tx_handle, samples,
                                       num_samples * sizeof(int16_t),
                                       &bytes_written, timeout_ms);
    if (samples_written) {
        *samples_written = bytes_written / sizeof(int16_t);
    }
    return ret;
}

esp_err_t audio_set_sample_rate(int rate)
{
    if (rate == s_sample_rate) return ESP_OK;

    ESP_LOGI(TAG, "Changing sample rate %d → %d", s_sample_rate, rate);

    // Disable, reconfigure, re-enable I2S
    ESP_ERROR_CHECK(i2s_channel_disable(s_tx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK,
            .bclk = I2S_BCK,
            .ws   = I2S_WS,
            .dout = I2S_DOUT,
            .din  = I2S_DIN,
            .invert_flags = { false, false, false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(s_tx_handle, &std_cfg.clk_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_handle));

    // Reconfigure ES8311 clocks
    ESP_ERROR_CHECK(es8311_sample_frequency_config(
        s_codec, rate * MCLK_MULTIPLE, rate));

    s_sample_rate = rate;
    return ESP_OK;
}

esp_err_t audio_set_volume(int vol)
{
    return es8311_voice_volume_set(s_codec, vol, NULL);
}

void audio_pa_enable(bool on)
{
    gpio_set_level(PA_EN, on ? 1 : 0);
}

#include <math.h>

void audio_test_tone(int duration_ms)
{
    ESP_LOGI(TAG, "Playing 440 Hz test tone for %d ms", duration_ms);
    const int freq = 440;
    const int rate = s_sample_rate;
    // 1024 stereo samples per chunk
    int16_t buf[2048];
    int total_samples = (rate * duration_ms) / 1000;
    int phase = 0;

    while (total_samples > 0) {
        int chunk = 1024;
        if (chunk > total_samples) chunk = total_samples;
        for (int i = 0; i < chunk; i++) {
            int16_t val = (int16_t)(16000.0f * sinf(2.0f * M_PI * freq * phase / rate));
            buf[i * 2 + 0] = val;  // left
            buf[i * 2 + 1] = val;  // right
            phase++;
        }
        size_t written;
        i2s_channel_write(s_tx_handle, buf, chunk * 4, &written, 1000);
        total_samples -= chunk;
    }
    ESP_LOGI(TAG, "Test tone done");
}
