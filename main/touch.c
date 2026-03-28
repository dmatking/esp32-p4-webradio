// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// GT911 touch controller — direct I2C using legacy driver.
// Bypasses esp_lcd_panel_io because v1 (legacy) doesn't handle
// GT911's 16-bit register addressing correctly.

#include "touch.h"
#include "display.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include <string.h>

#define TAG "touch"

#define GT911_ADDR       0x5D
#define GT911_ADDR_ALT   0x14
#define I2C_PORT         I2C_NUM_0

// GT911 registers (16-bit)
#define GT911_PRODUCT_ID   0x8140
#define GT911_CONFIG       0x8047
#define GT911_X_RES_L      0x8048
#define GT911_X_RES_H      0x8049
#define GT911_Y_RES_L      0x804A
#define GT911_Y_RES_H      0x804B
#define GT911_READ_XY      0x814E
#define GT911_POINT1       0x814F  // track_id, Xlo, Xhi, Ylo, Yhi, Szlo, Szhi, reserved

// Thresholds
#define SWIPE_MIN_DY     80
#define SWIPE_MAX_DX     60
#define TAP_MAX_MOVE     20
#define DEBOUNCE_FRAMES  4

static uint8_t s_gt911_addr = 0;  // 0 = not initialized
static uint16_t s_gt911_x_res = 720;
static uint16_t s_gt911_y_res = 720;

// Gesture tracking
static bool     s_was_touching;
static uint16_t s_start_x, s_start_y;
static uint16_t s_last_x, s_last_y;
static int      s_debounce;

// Debug
static char s_touch_debug[64] = "no init";
static uint16_t s_debug_x, s_debug_y;
static uint16_t s_debug_raw_x, s_debug_raw_y;
static bool s_debug_touching;

const char *touch_debug_str(void) { return s_touch_debug; }
bool touch_debug_pos(uint16_t *x, uint16_t *y)
{
    *x = s_debug_x; *y = s_debug_y;
    return (s_debug_x != 0 || s_debug_y != 0);
}
bool touch_debug_raw(uint16_t *x, uint16_t *y)
{
    *x = s_debug_raw_x; *y = s_debug_raw_y;
    return (s_debug_raw_x != 0 || s_debug_raw_y != 0);
}

// Read GT911 register (16-bit address, variable length data)
static esp_err_t gt911_read(uint16_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_gt911_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (reg >> 8) & 0xFF, true);
    i2c_master_write_byte(cmd, reg & 0xFF, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_gt911_addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// Write single byte to GT911 register
static esp_err_t gt911_write_byte(uint16_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_gt911_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (reg >> 8) & 0xFF, true);
    i2c_master_write_byte(cmd, reg & 0xFF, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// Track consecutive I2C errors for recovery
static int s_i2c_errors = 0;

esp_err_t touch_init(void)
{
    // Try both GT911 addresses
    const uint8_t addrs[] = { GT911_ADDR, GT911_ADDR_ALT };

    for (int a = 0; a < 2; a++) {
        s_gt911_addr = addrs[a];
        uint8_t id[4] = {0};
        esp_err_t ret = gt911_read(GT911_PRODUCT_ID, id, 4);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "GT911 at 0x%02X, product ID: %c%c%c%c",
                     addrs[a], id[0], id[1], id[2], id[3]);

            // Read configured resolution
            uint8_t res[4] = {0};
            if (gt911_read(GT911_X_RES_L, res, 4) == ESP_OK) {
                s_gt911_x_res = res[0] | (res[1] << 8);
                s_gt911_y_res = res[2] | (res[3] << 8);
            }
            ESP_LOGI(TAG, "GT911 resolution: %dx%d (display: %dx%d)",
                     s_gt911_x_res, s_gt911_y_res, DISP_W, DISP_H);

            snprintf(s_touch_debug, sizeof(s_touch_debug),
                     "GT911 %dx%d", s_gt911_x_res, s_gt911_y_res);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "GT911 at 0x%02X: read failed (%s)", addrs[a], esp_err_to_name(ret));
    }

    s_gt911_addr = 0;
    snprintf(s_touch_debug, sizeof(s_touch_debug), "GT911 not found");
    ESP_LOGE(TAG, "GT911 not found at either address");
    return ESP_FAIL;
}

// Count of consecutive polls with no touch reported (buffer_ready + 0 points)
static int s_no_touch_count = 0;
#define RELEASE_THRESHOLD 2  // require 2 consecutive no-touch polls before triggering release

touch_event_t touch_poll(void)
{
    if (!s_gt911_addr) return TOUCH_EVENT_NONE;

    if (s_debounce > 0) {
        s_debounce--;
        return TOUCH_EVENT_NONE;
    }

    // Too many consecutive I2C errors — back off heavily
    if (s_i2c_errors > 5) {
        s_i2c_errors--;  // slowly recover (takes s_i2c_errors polls to resume)
        return TOUCH_EVENT_NONE;
    }

    // Read touch status register
    uint8_t status = 0;
    if (gt911_read(GT911_READ_XY, &status, 1) != ESP_OK) {
        s_i2c_errors++;
        return TOUCH_EVENT_NONE;
    }
    s_i2c_errors = 0;

    uint8_t num_points = status & 0x0F;
    bool buffer_ready = (status & 0x80) != 0;

    if (buffer_ready) {
        gt911_write_byte(GT911_READ_XY, 0);
    }

    // If GT911 hasn't refreshed (buffer_ready=0), don't change touch state
    if (!buffer_ready) {
        return TOUCH_EVENT_NONE;
    }

    if (num_points > 0) {
        s_no_touch_count = 0;
        uint8_t point[8] = {0};
        if (gt911_read(GT911_POINT1, point, 8) == ESP_OK) {
            // point[0]=track_id, point[1]=Xlo, point[2]=Xhi, point[3]=Ylo, point[4]=Yhi
            uint16_t raw_x = point[1] | (point[2] << 8);
            uint16_t raw_y = point[3] | (point[4] << 8);
            s_debug_raw_x = raw_x;
            s_debug_raw_y = raw_y;
            // Scale from GT911 resolution to display resolution
            uint16_t x, y;
            if (s_gt911_x_res > 0 && s_gt911_y_res > 0 &&
                (s_gt911_x_res != DISP_W || s_gt911_y_res != DISP_H)) {
                x = (uint16_t)((uint32_t)raw_x * DISP_W / s_gt911_x_res);
                y = (uint16_t)((uint32_t)raw_y * DISP_H / s_gt911_y_res);
            } else {
                x = raw_x;
                y = raw_y;
            }
            if (x >= DISP_W) x = DISP_W - 1;
            if (y >= DISP_H) y = DISP_H - 1;

            s_debug_touching = true;
            s_debug_x = x;
            s_debug_y = y;

            if (!s_was_touching) {
                s_start_x = x;
                s_start_y = y;
                s_was_touching = true;
            }
            s_last_x = x;
            s_last_y = y;
        }
        return TOUCH_EVENT_NONE;
    }

    // buffer_ready=true, num_points=0: finger might be up
    s_debug_touching = false;
    if (!s_was_touching) return TOUCH_EVENT_NONE;

    // Require several consecutive no-touch reads before triggering release
    s_no_touch_count++;
    if (s_no_touch_count < RELEASE_THRESHOLD) return TOUCH_EVENT_NONE;

    // Confirmed release
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
        if (s_start_x < third) {
            ev = TOUCH_EVENT_TAP_LEFT;
        } else if (s_start_x > third * 2) {
            ev = TOUCH_EVENT_TAP_RIGHT;
        } else {
            ev = TOUCH_EVENT_TAP_CENTER;
        }
    }

    if (ev != TOUCH_EVENT_NONE) {
        s_debounce = DEBOUNCE_FRAMES;
        ESP_LOGI(TAG, "Touch: %d (%d,%d)->(%d,%d)",
                 ev, s_start_x, s_start_y, s_last_x, s_last_y);
    }
    return ev;
}
