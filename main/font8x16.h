// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Minimal 8x16 bitmap font for ASCII 32–126.
// Each character is 16 bytes (one byte per row, MSB = leftmost pixel).
// Public domain font data based on the classic VGA/BIOS font.

#pragma once

#include <stdint.h>
#include "display.h"

#define FONT_W 8
#define FONT_H 16

extern const uint8_t font8x16_data[];

// Draw a single character at (x,y) in BGR color onto the display backbuffer.
static inline void font_putc(uint8_t *fb, int x, int y, char c,
                              uint8_t b, uint8_t g, uint8_t r)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = &font8x16_data[(c - 32) * FONT_H];
    for (int row = 0; row < FONT_H; row++) {
        int py = y + row;
        if (py < 0 || py >= DISP_H) continue;
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_W; col++) {
            if (!(bits & (0x80 >> col))) continue;
            int px = x + col;
            if (px < 0 || px >= DISP_W) continue;
            int off = (py * DISP_W + px) * DISP_BPP;
            fb[off + 0] = b;
            fb[off + 1] = g;
            fb[off + 2] = r;
        }
    }
}

// Draw a string. Returns the x position after the last character.
static inline int font_puts(uint8_t *fb, int x, int y, const char *s,
                             uint8_t b, uint8_t g, uint8_t r)
{
    while (*s) {
        font_putc(fb, x, y, *s++, b, g, r);
        x += FONT_W;
    }
    return x;
}

// Draw a string scaled 2x (16x32 per char).
static inline void font_puts_2x(uint8_t *fb, int x, int y, const char *s,
                                  uint8_t b, uint8_t g, uint8_t r)
{
    while (*s) {
        char c = *s++;
        if (c < 32 || c > 126) c = '?';
        const uint8_t *glyph = &font8x16_data[(c - 32) * FONT_H];
        for (int row = 0; row < FONT_H; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < FONT_W; col++) {
                if (!(bits & (0x80 >> col))) continue;
                // Draw 2x2 block
                for (int dy = 0; dy < 2; dy++) {
                    int py = y + row * 2 + dy;
                    if (py < 0 || py >= DISP_H) continue;
                    for (int dx = 0; dx < 2; dx++) {
                        int px = x + col * 2 + dx;
                        if (px < 0 || px >= DISP_W) continue;
                        int off = (py * DISP_W + px) * DISP_BPP;
                        fb[off + 0] = b;
                        fb[off + 1] = g;
                        fb[off + 2] = r;
                    }
                }
            }
        }
        x += FONT_W * 2;
    }
}
