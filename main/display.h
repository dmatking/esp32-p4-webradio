// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Display driver for Waveshare ESP32-P4 720x720 LCD (ST7703, MIPI-DSI)
// Ported from p4-demos with double-buffered PPA SRM async flush.

#pragma once
#include <stdint.h>

// Display dimensions are board-dependent (set by the active board's
// display_init): 720x720 on the Waveshare panel, 1280x720 on the Tab5.
#define DISP_BPP 2                 // RGB565, 16bpp
extern int g_disp_w;
extern int g_disp_h;
#define DISP_W   g_disp_w
#define DISP_H   g_disp_h
#define DISP_FB_SIZE (g_disp_w * g_disp_h * DISP_BPP)

// The backbuf holds one little-endian uint16 RGB565 pixel per location.
static inline uint16_t disp_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void     display_init(void);

// Start async DMA copy of current backbuf → hardware framebuffer.
// Swaps to the other backbuf so rendering can continue immediately.
void     display_flush(void);

// Block until any in-flight flush completes.
void     display_flush_wait(void);

// Return the current render backbuf (write pixels here before calling flush).
uint8_t *display_backbuf(void);

// Convenience: fill entire backbuf with a solid RGB color.
void     display_fill(uint8_t r, uint8_t g, uint8_t b);

// Set a single pixel in the current backbuf.
static inline void display_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    extern uint8_t *g_backbuf;
    ((uint16_t *)g_backbuf)[y * DISP_W + x] = disp_rgb565(r, g, b);
}
