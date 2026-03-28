// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Display driver for Waveshare ESP32-P4 720x720 LCD (ST7703, MIPI-DSI)
// Ported from p4-demos with double-buffered PPA SRM async flush.

#pragma once
#include <stdint.h>

#define DISP_W   720
#define DISP_H   720
#define DISP_BPP 3
#define DISP_FB_SIZE (DISP_W * DISP_H * DISP_BPP)

// Pixel format in the backbuf is BGR (bytes: [0]=B, [1]=G, [2]=R)

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
    uint8_t *p = g_backbuf + (y * DISP_W + x) * DISP_BPP;
    p[0] = b; p[1] = g; p[2] = r;
}
