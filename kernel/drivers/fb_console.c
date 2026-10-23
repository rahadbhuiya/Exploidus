#include "fb_console.h"
#include "fb.h"
#include "font.h"
#include <string.h>

static inline uint32_t fb_cols(void)
{
    return (g_fb.width && FONT_W) ? (g_fb.width / FONT_W) : (800 / FONT_W);
}

static inline uint32_t fb_rows(void)
{
    return (g_fb.height && FONT_H) ? (g_fb.height / FONT_H) : (600 / FONT_H);
}

#define FG  0xFFFFFF
#define BG  0x000000

static uint32_t g_col = 0;
static uint32_t g_row = 0;
static int      g_enabled = 1;   /* 1 = text console active, 0 = GUI mode */

void fb_console_enable(void)  { g_enabled = 1; }
void fb_console_disable(void) { g_enabled = 0; }
int  fb_console_enabled(void) { return g_enabled; }

void fb_console_init(void)
{
    if (g_fb.active) {
        fb_clear(BG);
    }
    g_col = 0;
    g_row = 0;
}

static void scroll_up(void)
{
    uint32_t rows = fb_rows();
    uint8_t *fb = (uint8_t *)(uintptr_t)g_fb.addr;
    uint32_t row_bytes = g_fb.pitch * FONT_H;
    for (uint32_t r = 0; r < rows - 1; r++)
        memcpy(fb + r * row_bytes, fb + (r+1) * row_bytes, row_bytes);
    /* Clear last row */
    fb_fill_rect(0, (rows - 1) * FONT_H, g_fb.width, FONT_H, BG);
}

void fb_console_putc(char c)
{
    if (!g_fb.active) return;
    if (!g_enabled)   return;   /* GUI mode — suppress text output */

    uint32_t cols = fb_cols();
    uint32_t rows = fb_rows();

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
        if (g_col >= cols) {
            g_col = 0;
            g_row++;
        }
    }

    if (g_row >= rows) {
        scroll_up();
        g_row = rows - 1;
    }
}

void fb_console_puts(const char *s)
{
    while (*s) fb_console_putc(*s++);
}