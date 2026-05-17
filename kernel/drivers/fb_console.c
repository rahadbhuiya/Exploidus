#include "fb_console.h"
#include "fb.h"
#include "font.h"
#include <string.h>

#define COLS  (800 / FONT_W)   /* 100 columns */
#define ROWS  (600 / FONT_H)   /* 37 rows     */

#define FG  0xFFFFFF
#define BG  0x000000

static uint32_t g_col = 0;
static uint32_t g_row = 0;

void fb_console_init(void)
{
    fb_clear(BG);
    g_col = 0;
    g_row = 0;
}

static void scroll_up(void)
{
    /* Copy rows 1..ROWS-1 up by one */
    uint8_t *fb = (uint8_t *)(uintptr_t)g_fb.addr;
    uint32_t row_bytes = g_fb.pitch * FONT_H;
    for (uint32_t r = 0; r < ROWS - 1; r++)
        memcpy(fb + r * row_bytes, fb + (r+1) * row_bytes, row_bytes);
    /* Clear last row */
    fb_fill_rect(0, (ROWS-1)*FONT_H, g_fb.width, FONT_H, BG);
}

void fb_console_putc(char c)
{
    if (!g_fb.active) return;

    if (c == '\n') {
        g_col = 0;
        g_row++;
    } else if (c == '\r') {
        g_col = 0;
    } else if (c == '\b') {
        if (g_col > 0) {
            g_col--;
            font_draw_char(g_col * FONT_W, g_row * FONT_H, ' ', FG, BG);
        }
    } else {
        font_draw_char(g_col * FONT_W, g_row * FONT_H, c, FG, BG);
        g_col++;
        if (g_col >= COLS) {
            g_col = 0;
            g_row++;
        }
    }

    if (g_row >= ROWS) {
        scroll_up();
        g_row = ROWS - 1;
    }
}

void fb_console_puts(const char *s)
{
    while (*s) fb_console_putc(*s++);
}
