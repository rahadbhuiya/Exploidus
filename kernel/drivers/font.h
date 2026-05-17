#pragma once
#include <stdint.h>

/* 8x16 bitmap font — draws text directly to framebuffer */
void font_init(void);
void font_draw_char(uint32_t x, uint32_t y, char c,
                    uint32_t fg, uint32_t bg);
void font_draw_str(uint32_t x, uint32_t y, const char *s,
                   uint32_t fg, uint32_t bg);

#define FONT_W  8
#define FONT_H  16
