// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Display driver — ported from p4-demos/main/main.c

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_st7703.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
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

// Hardware double-buffering: the DPI controller owns two framebuffers in
// PSRAM. We render directly into the back one (g_backbuf) and hand it to the
// driver, which swaps it in on the next vsync and scans it out. No PPA copy
// and no separate backbuffer — see esp32-video-stream/main/board_p4_ev.c.
static uint8_t *s_fb[2];    // the two DPI framebuffers
static int      s_back;     // index of the buffer we render into
uint8_t        *g_backbuf;  // current render buffer (public for inline set_pixel)

static esp_lcd_panel_handle_t panel_handle;

void display_flush_wait(void)
{
    // No-op: with num_fbs=2 the driver's draw_bitmap blocks on the buffer
    // swap, so there is never a flush to wait on separately.
}

void display_flush(void)
{
    esp_cache_msync(g_backbuf, DISP_FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    // Queues this buffer as the new active fb; blocks until the previous
    // frame's buffer is free, giving tear-free double buffering + pacing.
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, DISP_W, DISP_H, g_backbuf);
    s_back ^= 1;
    g_backbuf = s_fb[s_back];
}

uint8_t *display_backbuf(void)
{
    return g_backbuf;
}

void display_fill(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t px = disp_rgb565(r, g, b);
    uint16_t *buf = (uint16_t *)g_backbuf;
    for (int i = 0; i < DISP_W * DISP_H; i++) buf[i] = px;
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
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 2,
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
        .bits_per_pixel = 16,
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

    // Get the two hardware framebuffers (scanned by DSI directly from PSRAM)
    void *fb0 = NULL, *fb1 = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel_handle, 2, &fb0, &fb1));
    s_fb[0] = (uint8_t *)fb0;
    s_fb[1] = (uint8_t *)fb1;
    memset(s_fb[0], 0, DISP_FB_SIZE);
    memset(s_fb[1], 0, DISP_FB_SIZE);
    s_back = 0;
    g_backbuf = s_fb[s_back];

    ESP_LOGI(TAG, "Display init done: %dx%d RGB565, MIPI-DSI, hw double-buffered", DISP_W, DISP_H);
}
