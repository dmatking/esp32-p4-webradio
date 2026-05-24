// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Board: M5Stack Tab5 (ESP32-P4), post-Oct-2025 hardware revision.
//   Display : 720x1280 MIPI-DSI 2-lane (ST7123), RGB565, hw double-buffered
//   Touch   : ST7123 capacitive (I2C 0x55)
//   Audio   : ES8388 codec via esp_codec_dev + I2S
//   Power   : two PI4IOE5V6408 I2C IO expanders gate LCD/touch reset, the
//             speaker amp, and the ESP32-C6 WLAN rail.
//
// Implements display.h, audio.h, and board.h for webradio's render path.
// Display init sequence + power sequencing derived from M5Tab5-UserDemo (MIT,
// M5Stack) and our m5stack-tab5-video-stream board bring-up.
//
// NOTE: brought up in the panel's native portrait orientation (720x1280).
// Landscape (1280x720) layout is handled separately.

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_st7123.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_ldo_regulator.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8388_codec.h"

#include "display.h"
#include "audio.h"
#include "board.h"

static const char *TAG = "board_tab5";

const char *board_name(void) { return "M5Stack Tab5"; }

// ── Pins / addresses ─────────────────────────────────────────────────────
#define I2C_BUS_NUM     0
#define I2C_SDA_GPIO    31
#define I2C_SCL_GPIO    32
#define I2C_TIMEOUT_MS  50

#define PI4IOE1_ADDR    0x43
#define PI4IOE2_ADDR    0x44
#define PI4IO_REG_CHIP_RESET 0x01
#define PI4IO_REG_IO_DIR     0x03
#define PI4IO_REG_OUT_SET    0x05
#define PI4IO_REG_OUT_H_IM   0x07
#define PI4IO_REG_IN_DEF_STA 0x09
#define PI4IO_REG_PULL_EN    0x0B
#define PI4IO_REG_PULL_SEL   0x0D
#define PI4IO_REG_INT_MASK   0x11

#define I2S_PORT        I2S_NUM_0
#define I2S_MCLK_GPIO   30
#define I2S_BCLK_GPIO   27
#define I2S_LRCK_GPIO   29
#define I2S_DOUT_GPIO   26

#define DSI_PHY_LDO_CHAN       3
#define DSI_PHY_LDO_VOLTAGE_MV 2500

#define LCD_BACKLIGHT_GPIO 22
#define LCD_LEDC_TIMER     LEDC_TIMER_0
#define LCD_LEDC_CHAN      LEDC_CHANNEL_1
#define LCD_LEDC_FREQ_HZ   5000
#define LCD_LEDC_DUTY_RES  LEDC_TIMER_12_BIT
#define LCD_LEDC_DUTY_MAX  4095

// The panel is physically portrait (720x1280). We present landscape by
// rendering into a logical 1280x720 buffer and rotating 90 deg into the
// physical framebuffer with the PPA (hardware 2D engine) every flush.
#define PHYS_W           720
#define PHYS_H           1280
#define LOG_W            1280   // logical landscape width
#define LOG_H            720    // logical landscape height
#define FB_BYTES         (PHYS_W * PHYS_H * 2)  // == LOG_W*LOG_H*2
#define DSI_LANE_BITRATE 965   // Mbps
#define DPI_CLOCK_MHZ    70

#define ST7123_TOUCH_I2C_ADDR 0x55
#define ST7123_TOUCH_INT_GPIO GPIO_NUM_23

#define DEFAULT_RATE 44100

// Runtime display dimensions (declared in display.h).
int g_disp_w;
int g_disp_h;

static esp_lcd_panel_handle_t   s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static uint8_t                 *s_fb[2];      // physical HW framebuffers (720x1280)
static int                      s_back;
static uint8_t                 *s_logical;    // logical landscape buffer (1280x720)
static ppa_client_handle_t      s_ppa;
uint8_t                        *g_backbuf;    // == s_logical; render target

static i2c_master_bus_handle_t  s_i2c_bus;
static i2c_master_dev_handle_t  s_pi4ioe1;
static esp_codec_dev_handle_t   s_spk_dev;
static int                      s_sample_rate = DEFAULT_RATE;
static int                      s_out_vol = 60;   // last volume; re-applied after codec reopen
static esp_lcd_touch_handle_t   s_touch;

// ── ST7123 vendor init sequence (Tab5, post-Oct-2025) ────────────────────
static const st7123_lcd_init_cmd_t s_st7123_init[] = {
    {0x60, (uint8_t[]){0x71, 0x23, 0xa2}, 3, 0},
    {0x60, (uint8_t[]){0x71, 0x23, 0xa3}, 3, 0},
    {0x60, (uint8_t[]){0x71, 0x23, 0xa4}, 3, 0},
    {0xA4, (uint8_t[]){0x31}, 1, 0},
    {0xD7, (uint8_t[]){0x10, 0x0A, 0x10, 0x2A, 0x80, 0x80}, 6, 0},
    {0x90, (uint8_t[]){0x71, 0x23, 0x5A, 0x20, 0x24, 0x09, 0x09}, 7, 0},
    {0xA3, (uint8_t[]){0x80, 0x01, 0x88, 0x30, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00,
                       0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x4F, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
                       0x00, 0x00, 0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x6F, 0x58, 0x00, 0x00, 0x00, 0xFF},
     40, 0},
    {0xA6, (uint8_t[]){0x03, 0x00, 0x24, 0x55, 0x36, 0x00, 0x39, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0x24,
                       0x55, 0x38, 0x00, 0x37, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0x24, 0x11, 0x00, 0x00,
                       0x00, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0xEC, 0x11, 0x00, 0x03, 0x00, 0x03, 0x6E,
                       0x6E, 0xFF, 0xFF, 0x00, 0x08, 0x80, 0x08, 0x80, 0x06, 0x00, 0x00, 0x00, 0x00},
     55, 0},
    {0xA7, (uint8_t[]){0x19, 0x19, 0x80, 0x64, 0x40, 0x07, 0x16, 0x40, 0x00, 0x44, 0x03, 0x6E, 0x6E, 0x91, 0xFF,
                       0x08, 0x80, 0x64, 0x40, 0x25, 0x34, 0x40, 0x00, 0x02, 0x01, 0x6E, 0x6E, 0x91, 0xFF, 0x08,
                       0x80, 0x64, 0x40, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x08, 0x80,
                       0x64, 0x40, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x6E, 0x6E, 0x84, 0xFF, 0x08, 0x80, 0x44},
     60, 0},
    {0xAC, (uint8_t[]){0x03, 0x19, 0x19, 0x18, 0x18, 0x06, 0x13, 0x13, 0x11, 0x11, 0x08, 0x08, 0x0A, 0x0A, 0x1C,
                       0x1C, 0x07, 0x07, 0x00, 0x00, 0x02, 0x02, 0x01, 0x19, 0x19, 0x18, 0x18, 0x06, 0x12, 0x12,
                       0x10, 0x10, 0x09, 0x09, 0x0B, 0x0B, 0x1C, 0x1C, 0x07, 0x07, 0x03, 0x03, 0x01, 0x01},
     44, 0},
    {0xAD, (uint8_t[]){0xF0, 0x00, 0x46, 0x00, 0x03, 0x50, 0x50, 0xFF, 0xFF, 0xF0, 0x40, 0x06, 0x01,
                       0x07, 0x42, 0x42, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF},
     25, 0},
    {0xAE, (uint8_t[]){0xFE, 0x3F, 0x3F, 0xFE, 0x3F, 0x3F, 0x00}, 7, 0},
    {0xB2, (uint8_t[]){0x15, 0x19, 0x05, 0x23, 0x49, 0xAF, 0x03, 0x2E, 0x5C, 0xD2, 0xFF, 0x10, 0x20,
                       0xFD, 0x20, 0xC0, 0x00}, 17, 0},
    {0xE8, (uint8_t[]){0x20, 0x6F, 0x04, 0x97, 0x97, 0x3E, 0x04, 0xDC, 0xDC, 0x3E, 0x06, 0xFA, 0x26, 0x3E}, 14, 0},
    {0x75, (uint8_t[]){0x03, 0x04}, 2, 0},
    {0xE7, (uint8_t[]){0x3B, 0x00, 0x00, 0x7C, 0xA1, 0x8C, 0x20, 0x1A, 0xF0, 0xB1, 0x50, 0x00,
                       0x50, 0xB1, 0x50, 0xB1, 0x50, 0xD8, 0x00, 0x55, 0x00, 0xB1, 0x00, 0x45,
                       0xC9, 0x6A, 0xFF, 0x5A, 0xD8, 0x18, 0x88, 0x15, 0xB1, 0x01, 0x01, 0x77},
     36, 0},
    {0xEA, (uint8_t[]){0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x2C}, 8, 0},
    {0xB0, (uint8_t[]){0x22, 0x43, 0x11, 0x61, 0x25, 0x43, 0x43}, 7, 0},
    {0xb7, (uint8_t[]){0x00, 0x00, 0x73, 0x73}, 4, 0},
    {0xBF, (uint8_t[]){0xA6, 0xAA}, 2, 0},
    {0xA9, (uint8_t[]){0x00, 0x00, 0x73, 0xFF, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03}, 10, 0},
    {0xC8, (uint8_t[]){0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05, 0x10, 0xF2, 0x06,
                       0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01, 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32,
                       0xDC, 0x09, 0x33, 0x0F, 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF},
     37, 0},
    {0xC9, (uint8_t[]){0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05, 0x10, 0xF2, 0x06,
                       0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01, 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32,
                       0xDC, 0x09, 0x33, 0x0F, 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF},
     37, 0},
    {0x36, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 100},
    {0x29, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 100},
};

// ── PI4IOE5V6408 IO expanders: power rails + LCD/touch reset ──────────────
static void pi4ioe_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port   = I2C_BUS_NUM,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    i2c_master_dev_handle_t dev2;
    i2c_device_config_t dev_cfg1 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PI4IOE1_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg1, &s_pi4ioe1));

    uint8_t wb[2], rb[1];
    // PI4IOE1: P1=SPK_EN, P2=EXT5V, P4=LCD_RST, P5=TP_RST, P6=CAM_RST
    wb[0] = PI4IO_REG_CHIP_RESET; wb[1] = 0xFF;
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_CHIP_RESET;
    i2c_master_transmit_receive(s_pi4ioe1, wb, 1, rb, 1, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_IO_DIR; wb[1] = 0b01111111;
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_OUT_H_IM; wb[1] = 0b00000000;
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_PULL_SEL; wb[1] = 0b01111111;
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_PULL_EN; wb[1] = 0b01111111;
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);
    // EXT5V(P2), TP_RST(P5), CAM_RST(P6) high; LCD_RST(P4), SPK_EN(P1) low
    wb[0] = PI4IO_REG_OUT_SET; wb[1] = 0b01100100;
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(10));
    // Release LCD_RST (P4): clean low->high pulse before the panel takes commands
    wb[0] = PI4IO_REG_OUT_SET; wb[1] = 0b01110100;
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(120));

    i2c_device_config_t dev_cfg2 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PI4IOE2_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg2, &dev2));

    // PI4IOE2: P0=WLAN_PWR_EN (the ESP32-C6 rail), P3=USB5V_EN, P7=CHG_EN
    wb[0] = PI4IO_REG_CHIP_RESET; wb[1] = 0xFF;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_CHIP_RESET;
    i2c_master_transmit_receive(dev2, wb, 1, rb, 1, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_IO_DIR; wb[1] = 0b10111001;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_OUT_H_IM; wb[1] = 0b00000110;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_PULL_SEL; wb[1] = 0b10111001;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_PULL_EN; wb[1] = 0b11111001;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_IN_DEF_STA; wb[1] = 0b01000000;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
    wb[0] = PI4IO_REG_INT_MASK; wb[1] = 0b10111111;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
    // WLAN_PWR_EN(P0), USB5V_EN(P3) high
    wb[0] = PI4IO_REG_OUT_SET; wb[1] = 0b00001001;
    i2c_master_transmit(dev2, wb, 2, I2C_TIMEOUT_MS);
}

// Set the speaker amp enable (PI4IOE1 P1).
static void spk_enable(bool on)
{
    if (!s_pi4ioe1) return;
    uint8_t wb[2] = { PI4IO_REG_OUT_SET, on ? 0b01110110 : 0b01110100 };
    i2c_master_transmit(s_pi4ioe1, wb, 2, I2C_TIMEOUT_MS);
}

// ── Display (display.h) ──────────────────────────────────────────────────
void display_flush_wait(void) { }

void display_flush(void)
{
    // Flush the rendered logical buffer out of cache, then let the PPA rotate
    // it 90 deg into the physical (portrait) back framebuffer.
    esp_cache_msync(s_logical, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ppa_srm_oper_config_t srm = {
        .in = {
            .buffer       = s_logical,
            .pic_w        = LOG_W,
            .pic_h        = LOG_H,
            .block_w      = LOG_W,
            .block_h      = LOG_H,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm       = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer       = s_fb[s_back],
            .buffer_size  = FB_BYTES,
            .pic_w        = PHYS_W,
            .pic_h        = PHYS_H,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm       = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ppa_do_scale_rotate_mirror(s_ppa, &srm);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PHYS_W, PHYS_H, s_fb[s_back]);
    s_back ^= 1;
}

uint8_t *display_backbuf(void) { return g_backbuf; }

void display_fill(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t px = disp_rgb565(r, g, b);
    uint16_t *buf = (uint16_t *)s_logical;
    int n = LOG_W * LOG_H;
    for (int i = 0; i < n; i++) buf[i] = px;
}

void display_init(void)
{
    g_disp_w = LOG_W;   // render in logical landscape
    g_disp_h = LOG_H;

    // IO expanders bring up power rails and pulse LCD_RST. Also powers the
    // ESP32-C6 WLAN rail — must run before esp_hosted_init in the network task.
    pi4ioe_init();

    esp_ldo_channel_handle_t phy_ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &phy_ldo));

    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_LEDC_DUTY_RES,
        .timer_num = LCD_LEDC_TIMER,
        .freq_hz = LCD_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    const ledc_channel_config_t bl_chan = {
        .gpio_num = LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CHAN,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_LEDC_TIMER,
        .duty = 0, .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_chan));

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t dsi_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_BITRATE,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&dsi_cfg, &dsi_bus));

    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &s_panel_io));

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DPI_CLOCK_MHZ,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = PHYS_W, .v_size = PHYS_H,
            .hsync_pulse_width = 2, .hsync_back_porch = 40, .hsync_front_porch = 40,
            .vsync_pulse_width = 2, .vsync_back_porch = 8,  .vsync_front_porch = 220,
        },
        .flags.use_dma2d = true,
    };

    st7123_vendor_config_t vendor_cfg = {
        .init_cmds = s_st7123_init,
        .init_cmds_size = sizeof(s_st7123_init) / sizeof(s_st7123_init[0]),
        .mipi_config = { .dsi_bus = dsi_bus, .dpi_config = &dpi_cfg, .lane_num = 2 },
    };
    const esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 24,
        .vendor_config = (void *)&vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7123(s_panel_io, &dev_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    void *fb0 = NULL, *fb1 = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1));
    s_fb[0] = (uint8_t *)fb0;
    s_fb[1] = (uint8_t *)fb1;
    memset(s_fb[0], 0, FB_BYTES);
    esp_cache_msync(s_fb[0], FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    memset(s_fb[1], 0, FB_BYTES);
    esp_cache_msync(s_fb[1], FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    s_back = 0;

    // Logical landscape render buffer (the shared render code draws here).
    s_logical = heap_caps_aligned_calloc(64, FB_BYTES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    assert(s_logical);
    g_backbuf = s_logical;

    // PPA client for the per-frame 90-degree rotate (logical -> physical fb).
    ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &s_ppa));

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CHAN, LCD_LEDC_DUTY_MAX);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CHAN);

    ESP_LOGI(TAG, "Display init: logical %dx%d -> physical %dx%d (PPA 90deg)",
             LOG_W, LOG_H, PHYS_W, PHYS_H);
}

// ── Audio (audio.h) — ES8388 via esp_codec_dev ───────────────────────────
static const audio_codec_data_if_t *s_data_if;

static esp_err_t codec_open(int rate)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = (uint32_t)rate,
    };
    return esp_codec_dev_open(s_spk_dev, &fs) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_init(void)
{
    spk_enable(true);

    i2s_chan_handle_t tx_chan;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO, .bclk = I2S_BCLK_GPIO, .ws = I2S_LRCK_GPIO,
            .dout = I2S_DOUT_GPIO, .din = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = (uint8_t)I2S_PORT,
        .tx_handle = tx_chan,
        .rx_handle = NULL,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (uint8_t)I2C_BUS_NUM,
        .addr = ES8388_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8388_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    const audio_codec_if_t *codec_if = es8388_codec_new(&codec_cfg);
    if (!codec_if) { ESP_LOGE(TAG, "ES8388 codec create failed"); return ESP_FAIL; }

    esp_codec_dev_cfg_t cdev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = s_data_if,
    };
    s_spk_dev = esp_codec_dev_new(&cdev_cfg);
    if (!s_spk_dev) { ESP_LOGE(TAG, "codec_dev create failed"); return ESP_FAIL; }

    if (codec_open(DEFAULT_RATE) != ESP_OK) { ESP_LOGE(TAG, "codec open failed"); return ESP_FAIL; }
    esp_codec_dev_set_out_vol(s_spk_dev, s_out_vol);
    s_sample_rate = DEFAULT_RATE;
    ESP_LOGI(TAG, "Audio init: ES8388 @ I2C 0x%02x, I2S%d, %d Hz stereo",
             ES8388_CODEC_DEFAULT_ADDR, I2S_PORT, DEFAULT_RATE);
    return ESP_OK;
}

esp_err_t audio_write(const int16_t *samples, size_t num_samples,
                      size_t *samples_written, uint32_t timeout_ms)
{
    (void)timeout_ms;
    int ret = esp_codec_dev_write(s_spk_dev, (void *)samples, num_samples * sizeof(int16_t));
    if (samples_written) *samples_written = (ret == 0) ? num_samples : 0;
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_set_sample_rate(int rate)
{
    if (rate < 8000 || rate > 96000) {
        ESP_LOGW(TAG, "Ignoring invalid sample rate %d", rate);
        return ESP_ERR_INVALID_ARG;
    }
    if (rate == s_sample_rate) return ESP_OK;
    ESP_LOGI(TAG, "Changing sample rate %d -> %d", s_sample_rate, rate);
    esp_codec_dev_close(s_spk_dev);
    if (codec_open(rate) != ESP_OK) {
        ESP_LOGW(TAG, "reopen at %d failed; restoring %d", rate, s_sample_rate);
        codec_open(s_sample_rate);
        esp_codec_dev_set_out_vol(s_spk_dev, s_out_vol);
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_spk_dev, s_out_vol);  // reopen resets volume; restore it
    s_sample_rate = rate;
    return ESP_OK;
}

esp_err_t audio_set_volume(int vol)
{
    s_out_vol = vol;
    return esp_codec_dev_set_out_vol(s_spk_dev, vol) == 0 ? ESP_OK : ESP_FAIL;
}

void audio_pa_enable(bool on)
{
    spk_enable(on);
}

void audio_test_tone(int duration_ms)
{
    const int freq = 440, rate = s_sample_rate;
    int16_t buf[2048];
    int total = (rate * duration_ms) / 1000, phase = 0;
    while (total > 0) {
        int chunk = 1024;
        if (chunk > total) chunk = total;
        for (int i = 0; i < chunk; i++) {
            int16_t v = (int16_t)(16000.0f * sinf(2.0f * M_PI * freq * phase / rate));
            buf[i*2] = v; buf[i*2+1] = v; phase++;
        }
        size_t w;
        audio_write(buf, chunk * 2, &w, 1000);
        total -= chunk;
    }
}

// ── Touch — ST7123 (board.h raw read) ────────────────────────────────────
esp_err_t board_touch_init(void)
{
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .dev_addr = ST7123_TOUCH_I2C_ADDR,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .flags.disable_control_phase = 1,
    };
    if (esp_lcd_new_panel_io_i2c_v2(s_i2c_bus, &tp_io_cfg, &tp_io) != ESP_OK) {
        ESP_LOGE(TAG, "touch panel IO create failed");
        return ESP_FAIL;
    }
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = PHYS_W,   // raw panel coordinates; rotation applied below
        .y_max = PHYS_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = ST7123_TOUCH_INT_GPIO,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
    };
    esp_err_t err = esp_lcd_touch_new_i2c_st7123(tp_io, &tp_cfg, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ST7123 touch init failed (0x%x)", err);
        s_touch = NULL;
        return err;
    }
    ESP_LOGI(TAG, "ST7123 touch ready @ 0x%02x", ST7123_TOUCH_I2C_ADDR);
    return ESP_OK;
}

bool board_touch_read(uint16_t *x, uint16_t *y)
{
    if (!s_touch) return false;
    esp_lcd_touch_read_data(s_touch);
    uint16_t px[1], py[1], strength[1];
    uint8_t cnt = 0;
    bool pressed = esp_lcd_touch_get_coordinates(s_touch, px, py, strength, &cnt, 1);
    if (pressed && cnt > 0) {
        // Map raw portrait panel coords (px:0..PHYS_W, py:0..PHYS_H) into the
        // logical landscape surface (0..LOG_W, 0..LOG_H), matching the PPA 90deg
        // display rotation. If taps land mirrored, flip the two expressions.
        uint16_t rx = px[0], ry = py[0];
        uint16_t lx = ry;        // panel-y -> logical-x (0..1280): left/right correct
        uint16_t ly = rx;        // panel-x -> logical-y (0..720): not flipped (swipes)
        if (lx >= LOG_W) lx = LOG_W - 1;
        if (ly >= LOG_H) ly = LOG_H - 1;
        *x = lx;
        *y = ly;
        return true;
    }
    return false;
}
