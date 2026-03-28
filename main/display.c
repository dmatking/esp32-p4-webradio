// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Display driver — ported from p4-demos/main/main.c

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_st7703.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/ppa.h"
#include "display.h"

static const char *TAG = "display";

// MIPI-DSI config for Waveshare ST7703 720x720
#define DSI_LANE_NUM      2
#define DSI_LANE_MBPS     480
#define DSI_DPI_CLK_MHZ   38
#define DSI_PHY_LDO_CHAN  3
#define DSI_PHY_LDO_MV    2500
#define DSI_BK_LIGHT_GPIO 26
#define DSI_RST_GPIO      27

static uint8_t *fb;         // panel's live framebuffer (scanned by DSI hardware)
static uint8_t *backbuf_a;  // double-buffer A
static uint8_t *backbuf_b;  // double-buffer B
uint8_t        *g_backbuf;  // current render buffer (public for inline set_pixel)

static esp_lcd_panel_handle_t panel_handle;
static ppa_client_handle_t    ppa_srm_client;
static SemaphoreHandle_t      flush_done_sem;
static bool                   flush_pending;

// PPA SRM async completion callback — called from ISR
static bool flush_done_cb(ppa_client_handle_t client, ppa_event_data_t *event_data, void *user_data)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(flush_done_sem, &woken);
    return (woken == pdTRUE);
}

void display_flush_wait(void)
{
    if (flush_pending) {
        xSemaphoreTake(flush_done_sem, portMAX_DELAY);
        esp_cache_msync(fb, DISP_FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, DISP_W, DISP_H, fb);
        flush_pending = false;
    }
}

void display_flush(void)
{
    display_flush_wait();
    esp_cache_msync(g_backbuf, DISP_FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ppa_srm_oper_config_t srm_cfg = {
        .in = {
            .buffer = g_backbuf,
            .pic_w = DISP_W, .pic_h = DISP_H,
            .block_w = DISP_W, .block_h = DISP_H,
            .block_offset_x = 0, .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .out = {
            .buffer = fb,
            .buffer_size = DISP_FB_SIZE,
            .pic_w = DISP_W, .pic_h = DISP_H,
            .block_offset_x = 0, .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(ppa_srm_client, &srm_cfg));
    flush_pending = true;
    // Swap backbufs — CPU renders next frame while DMA copies current one
    g_backbuf = (g_backbuf == backbuf_a) ? backbuf_b : backbuf_a;
}

uint8_t *display_backbuf(void)
{
    return g_backbuf;
}

void display_fill(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *buf = g_backbuf;
    for (int i = 0; i < DISP_W * DISP_H; i++) {
        buf[i * DISP_BPP + 0] = b;
        buf[i * DISP_BPP + 1] = g;
        buf[i * DISP_BPP + 2] = r;
    }
}

void display_init(void)
{
    // Power MIPI PHY via LDO
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));

    // MIPI-DSI bus
    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));

    // DBI command IO (for ST7703 init commands)
    esp_lcd_panel_io_handle_t dbi_io;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io));

    // DPI video panel config
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DSI_DPI_CLK_MHZ,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
        .num_fbs = 1,
        .video_timing = {
            .h_size = DISP_W, .v_size = DISP_H,
            .hsync_back_porch = 50, .hsync_pulse_width = 20, .hsync_front_porch = 50,
            .vsync_back_porch = 20, .vsync_pulse_width = 4,  .vsync_front_porch = 20,
        },
        .flags = { .use_dma2d = true },
    };

    st7703_vendor_config_t vendor_cfg = {
        .flags = { .use_mipi_interface = 1 },
        .mipi_config = { .dsi_bus = dsi_bus, .dpi_config = &dpi_cfg },
    };
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = DSI_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7703(dbi_io, &dev_cfg, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // Backlight (active low on Waveshare board)
    gpio_config_t bk_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DSI_BK_LIGHT_GPIO,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_cfg));
    gpio_set_level(DSI_BK_LIGHT_GPIO, 0);

    // Get hardware framebuffer (scanned by DSI hardware directly from PSRAM)
    void *fb0 = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel_handle, 1, &fb0));
    fb = (uint8_t *)fb0;

    // Allocate double render buffers in PSRAM (64-byte aligned for DMA)
    backbuf_a = heap_caps_aligned_calloc(64, DISP_FB_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    backbuf_b = heap_caps_aligned_calloc(64, DISP_FB_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    assert(backbuf_a && backbuf_b);
    g_backbuf = backbuf_a;
    memset(fb, 0, DISP_FB_SIZE);

    // PPA SRM client for async backbuf→fb copy
    ppa_client_config_t srm_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_ERROR_CHECK(ppa_register_client(&srm_cfg, &ppa_srm_client));

    flush_done_sem = xSemaphoreCreateBinary();
    assert(flush_done_sem);
    ppa_event_callbacks_t srm_cbs = { .on_trans_done = flush_done_cb };
    ESP_ERROR_CHECK(ppa_client_register_event_callbacks(ppa_srm_client, &srm_cbs));

    // Kick off initial flush (black screen)
    display_flush();

    ESP_LOGI(TAG, "Display init done: %dx%d RGB888, MIPI-DSI, PPA SRM flush", DISP_W, DISP_H);
}
