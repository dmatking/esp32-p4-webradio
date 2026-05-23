// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Minimal 8x16 bitmap font for ASCII 32–126.
// Each character is 16 bytes (one byte per row, MSB = leftmost pixel).
// Public domain font data based on the classic VGA/BIOS font.

#pragma once

#include <stdint.h>
#include <string.h>
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
            ((uint16_t *)fb)[py * DISP_W + px] = disp_rgb565(r, g, b);
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
                        ((uint16_t *)fb)[py * DISP_W + px] = disp_rgb565(r, g, b);
                    }
                }
            }
        }
        x += FONT_W * 2;
    }
}

// Draw a string at an integer scale (each glyph pixel -> scale x scale block).
static inline int font_puts_scale(uint8_t *fb, int x, int y, const char *s,
                                   int scale, uint8_t b, uint8_t g, uint8_t r)
{
    uint16_t px565 = disp_rgb565(r, g, b);
    while (*s) {
        char c = *s++;
        if (c < 32 || c > 126) c = '?';
        const uint8_t *glyph = &font8x16_data[(c - 32) * FONT_H];
        for (int row = 0; row < FONT_H; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < FONT_W; col++) {
                if (!(bits & (0x80 >> col))) continue;
                for (int dy = 0; dy < scale; dy++) {
                    int py = y + row * scale + dy;
                    if (py < 0 || py >= DISP_H) continue;
                    int base = py * DISP_W;
                    for (int dx = 0; dx < scale; dx++) {
                        int px = x + col * scale + dx;
                        if (px < 0 || px >= DISP_W) continue;
                        ((uint16_t *)fb)[base + px] = px565;
                    }
                }
            }
        }
        x += FONT_W * scale;
    }
    return x;
}

// Pixel width of a string at a given scale.
static inline int font_text_w(const char *s, int scale)
{
    return (int)strlen(s) * FONT_W * scale;
}

// Draw a string horizontally centered on the display.
static inline void font_puts_center(uint8_t *fb, int y, const char *s, int scale,
                                     uint8_t b, uint8_t g, uint8_t r)
{
    int x = (DISP_W - font_text_w(s, scale)) / 2;
    if (x < 0) x = 0;
    font_puts_scale(fb, x, y, s, scale, b, g, r);
}
