#pragma once
#include <stdint.h>

void vga_init(void);
void vga_putc(char c);
void vga_puts(const char *s);
void vga_puthex(uint64_t val);
void vga_clear(void);
void vga_set_color(uint8_t fg, uint8_t bg);
