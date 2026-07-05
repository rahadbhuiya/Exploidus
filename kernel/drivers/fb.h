/*
 * fb.h — Exploidus Framebuffer Driver (improved)
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/*  framebuffer struct  */

typedef struct {
    uint64_t addr;
    uint32_t width, height, pitch;
    uint8_t  bpp;
    bool     active;
} framebuffer_t;

extern framebuffer_t g_fb;

/*  Exploidus Dark Theme colors  */

/* Backgrounds */
#define COL_BG_DESKTOP   0x0D1117   /* desktop wallpaper */
#define COL_BG_SURFACE   0x161B22   /* window body       */
#define COL_BG_ELEVATED  0x1F2937   /* title bar         */
#define COL_BG_HOVER     0x21262D   /* hover / selected  */

/* Borders */
#define COL_BORDER       0x30363D   /* window border     */
#define COL_BORDER_DIM   0x21262D   /* subtle divider    */

/* Text */
#define COL_TEXT_PRI     0xE6EDF3   /* primary text      */
#define COL_TEXT_SEC     0x8B949E   /* secondary / muted */
#define COL_TEXT_HINT    0x484F58   /* placeholder       */

/* Accents */
#define COL_ACCENT_PUR   0xA78BFA   /* terminal / primary */
#define COL_ACCENT_GRN   0x10B981   /* files / success    */
#define COL_ACCENT_BLU   0x58A6FF   /* info / system      */
#define COL_ACCENT_YEL   0xF0B429   /* warning            */

/* Traffic-light buttons */
#define COL_BTN_CLOSE    0xEF4444
#define COL_BTN_MIN      0xF59E0B
#define COL_BTN_MAX      0x10B981

/* Terminal foreground colors (ANSI-style) */
#define COL_TERM_BG      0x0D1117
#define COL_TERM_FG      0xE6EDF3
#define COL_TERM_GREEN   0x10B981
#define COL_TERM_BLUE    0x58A6FF
#define COL_TERM_YELLOW  0xF0B429
#define COL_TERM_RED     0xEF4444
#define COL_TERM_PURPLE  0xA78BFA
#define COL_TERM_CYAN    0x67E8F9
#define COL_TERM_GRAY    0x8B949E

/*  function declarations  */

/* init */
void     fb_init(uint64_t addr, uint32_t w, uint32_t h, uint32_t pitch, uint8_t bpp);

/* color helpers */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);
uint32_t fb_color_lighten(uint32_t color, uint8_t pct);

/* pixel */
void     fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void     fb_blend_pixel(uint32_t x, uint32_t y, uint32_t color, uint8_t alpha);

/* fill */
void     fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void     fb_clear(uint32_t color);
void     fb_draw_hline(int x, int y, int len, uint32_t color);
void     fb_draw_vline(int x, int y, int len, uint32_t color);

/* rounded rect */
void     fb_fill_rounded_rect(int x, int y, int w, int h, int r, uint32_t color);
void     fb_draw_rounded_rect(int x, int y, int w, int h, int r, uint32_t color);

/* circle */
void     fb_fill_circle(int cx, int cy, int r, uint32_t color);
void     fb_draw_circle(int cx, int cy, int r, uint32_t color);

/* shadow */
void     fb_draw_shadow(int x, int y, int w, int h, int r,
                        uint32_t shadow_color, uint8_t base_alpha);

/* window chrome */
int      fb_draw_window(int x, int y, int w, int h,
                        const char *title,
                        uint32_t bg_color,
                        uint32_t title_color,
                        uint32_t accent,
                        uint32_t text_color,
                        uint32_t border_color);

/* sidebar */
void     fb_draw_sidebar_btn(int sidebar_x, int y, char icon_char,
                             const char *label,
                             uint32_t icon_color,
                             uint32_t bg_color,
                             uint32_t hover_color,
                             int active);

/* taskbar */
int      fb_draw_taskbar_pill(int x, int y, const char *label,
                               uint32_t accent,
                               uint32_t bg_color,
                               int active);

/* line */
void     fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color);

/* double buffer */
int      fb_back_init(void);          /* boot time call */
void     fb_flip(void);               /* back→front copy             */
void     fb_blit(int32_t dst_x, int32_t dst_y, uint32_t w, uint32_t h,
                  const uint32_t *src, uint32_t bg_color);