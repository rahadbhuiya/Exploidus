/*
 * fb.c — Exploidus Framebuffer Driver (improved)
 * Adds: alpha blend, rounded rects, circles, shadows, hline/vline
 */

#include "fb.h"
#include "../drivers/serial.h"
#include "../mm/pmm.h"   
#include "../mm/kmalloc.h" 

framebuffer_t g_fb = {0};
static uint8_t *g_back_buf = NULL;

/*  init  */

void fb_init(uint64_t addr, uint32_t w, uint32_t h, uint32_t pitch, uint8_t bpp)
{
    g_fb.addr   = addr;
    g_fb.width  = w;
    g_fb.height = h;
    g_fb.pitch  = pitch;
    g_fb.bpp    = bpp;
    g_fb.active = true;
    serial_print("[FB] Framebuffer: ");
    serial_printhex((uint64_t)w);
    serial_print("x");
    serial_printhex((uint64_t)h);
    serial_print(" bpp=");
    serial_printhex((uint64_t)bpp);
    serial_print("\n");
}

/*  color helpers  */

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Lighten a color by pct (0–100). Useful for hover states. */
uint32_t fb_color_lighten(uint32_t color, uint8_t pct)
{
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >>  8) & 0xFF;
    uint8_t b =  color        & 0xFF;
    r = (uint8_t)(r + ((255 - r) * pct / 100));
    g = (uint8_t)(g + ((255 - g) * pct / 100));
    b = (uint8_t)(b + ((255 - b) * pct / 100));
    return fb_rgb(r, g, b);
}

/*  pixel primitives  */

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (!g_fb.active || x >= g_fb.width || y >= g_fb.height) return;
    if (g_back_buf) {
        uint32_t offset = y * (g_fb.pitch / 4) + x;
        ((uint32_t *)g_back_buf)[offset] = color;
    } else {
        uint32_t *pixel = (uint32_t *)(uintptr_t)
                          (g_fb.addr + y * g_fb.pitch + x * (g_fb.bpp / 8));
        *pixel = color;
    }
}

/*
 * Alpha-blend a pixel onto the framebuffer.
 * alpha = 255 → fully opaque   alpha = 0 → invisible
 * Uses integer math only — no floats needed.
 */
void fb_blend_pixel(uint32_t x, uint32_t y, uint32_t color, uint8_t alpha)
{
    if (!g_fb.active || x >= g_fb.width || y >= g_fb.height) return;
    if (alpha == 255) { fb_put_pixel(x, y, color); return; }
    if (alpha == 0)   return;

    uint32_t *pixel = g_back_buf
        ? (uint32_t *)(g_back_buf + y * g_fb.pitch + x * 4)
        : (uint32_t *)(uintptr_t)(g_fb.addr + y * g_fb.pitch + x * (g_fb.bpp / 8));
    uint32_t bg = *pixel;

    uint8_t sr = (color >> 16) & 0xFF;
    uint8_t sg = (color >>  8) & 0xFF;
    uint8_t sb =  color        & 0xFF;
    uint8_t br = (bg    >> 16) & 0xFF;
    uint8_t bg_ = (bg   >>  8) & 0xFF;
    uint8_t bb  =  bg          & 0xFF;

    uint8_t r = (uint8_t)((sr * alpha + br * (255 - alpha)) >> 8);
    uint8_t g = (uint8_t)((sg * alpha + bg_ * (255 - alpha)) >> 8);
    uint8_t b = (uint8_t)((sb * alpha + bb  * (255 - alpha)) >> 8);

    *pixel = fb_rgb(r, g, b);
}

/*  fill primitives  */

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    if (x >= g_fb.width || y >= g_fb.height) return;
    if (x + w > g_fb.width)  w = g_fb.width  - x;
    if (y + h > g_fb.height) h = g_fb.height - y;
    for (uint32_t row = y; row < y + h; row++) {
        uint32_t *dst = (uint32_t *)(g_back_buf
                        ? (g_back_buf + row * g_fb.pitch + x * 4)
                        : (uint8_t *)(uintptr_t)(g_fb.addr + row * g_fb.pitch + x * 4));
        for (uint32_t col = 0; col < w; col++)
            dst[col] = color;
    }
}

void fb_clear(uint32_t color)
{
    fb_fill_rect(0, 0, g_fb.width, g_fb.height, color);
}

/* Fast horizontal line — much quicker than fb_draw_line for h/v */
void fb_draw_hline(int x, int y, int len, uint32_t color)
{
    for (int i = 0; i < len; i++)
        fb_put_pixel((uint32_t)(x + i), (uint32_t)y, color);
}

void fb_draw_vline(int x, int y, int len, uint32_t color)
{
    for (int i = 0; i < len; i++)
        fb_put_pixel((uint32_t)x, (uint32_t)(y + i), color);
}

/*  rounded rectangle  */

/*
 * Filled rounded rectangle.
 * r = corner radius (suggest 4–8 for windows, 10–12 for large panels).
 * Uses integer circle equation — no sqrt, no floats.
 */
void fb_fill_rounded_rect(int x, int y, int w, int h, int r, uint32_t color)
{
    if (r <= 0 || r > w / 2 || r > h / 2) {
        fb_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, color);
        return;
    }

    /* center horizontal strip */
    fb_fill_rect((uint32_t)(x + r), (uint32_t)y,       (uint32_t)(w - 2 * r), (uint32_t)h,       color);
    /* left and right vertical strips */
    fb_fill_rect((uint32_t)x,       (uint32_t)(y + r), (uint32_t)r,            (uint32_t)(h - 2 * r), color);
    fb_fill_rect((uint32_t)(x + w - r), (uint32_t)(y + r), (uint32_t)r, (uint32_t)(h - 2 * r), color);

    /* four quarter-circle corners */
    for (int cy = 0; cy <= r; cy++) {
        for (int cx = 0; cx <= r; cx++) {
            if (cx * cx + cy * cy <= r * r) {
                fb_put_pixel((uint32_t)(x + r       - cx), (uint32_t)(y + r       - cy), color);
                fb_put_pixel((uint32_t)(x + w - r - 1 + cx), (uint32_t)(y + r       - cy), color);
                fb_put_pixel((uint32_t)(x + r       - cx), (uint32_t)(y + h - r - 1 + cy), color);
                fb_put_pixel((uint32_t)(x + w - r - 1 + cx), (uint32_t)(y + h - r - 1 + cy), color);
            }
        }
    }
}

/*
 * Outline-only rounded rectangle (border).
 * thickness = stroke width in pixels.
 */
void fb_draw_rounded_rect(int x, int y, int w, int h, int r, uint32_t color)
{
    /* straight edges */
    fb_draw_hline(x + r,     y,         w - 2 * r, color); /* top    */
    fb_draw_hline(x + r,     y + h - 1, w - 2 * r, color); /* bottom */
    fb_draw_vline(x,         y + r,     h - 2 * r, color); /* left   */
    fb_draw_vline(x + w - 1, y + r,     h - 2 * r, color); /* right  */

    /* corners using Bresenham circle */
    int f   = 1 - r;
    int ddx = 0;
    int ddy = -2 * r;
    int cx  = 0;
    int cy  = r;

    fb_put_pixel((uint32_t)(x + r),     (uint32_t)(y),         color);
    fb_put_pixel((uint32_t)(x + r),     (uint32_t)(y + h - 1), color);
    fb_put_pixel((uint32_t)(x + w - r - 1), (uint32_t)(y),     color);
    fb_put_pixel((uint32_t)(x + w - r - 1), (uint32_t)(y + h - 1), color);

    while (cx < cy) {
        if (f >= 0) { cy--; ddy += 2; f += ddy; }
        cx++; ddx += 2; f += ddx + 1;

        /* top-left */
        fb_put_pixel((uint32_t)(x + r - cx),     (uint32_t)(y + r - cy),     color);
        fb_put_pixel((uint32_t)(x + r - cy),     (uint32_t)(y + r - cx),     color);
        /* top-right */
        fb_put_pixel((uint32_t)(x + w - r + cx - 1), (uint32_t)(y + r - cy), color);
        fb_put_pixel((uint32_t)(x + w - r + cy - 1), (uint32_t)(y + r - cx), color);
        /* bottom-left */
        fb_put_pixel((uint32_t)(x + r - cx),     (uint32_t)(y + h - r + cy - 1), color);
        fb_put_pixel((uint32_t)(x + r - cy),     (uint32_t)(y + h - r + cx - 1), color);
        /* bottom-right */
        fb_put_pixel((uint32_t)(x + w - r + cx - 1), (uint32_t)(y + h - r + cy - 1), color);
        fb_put_pixel((uint32_t)(x + w - r + cy - 1), (uint32_t)(y + h - r + cx - 1), color);
    }
}

/*  circle  */

void fb_fill_circle(int cx, int cy, int r, uint32_t color)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                fb_put_pixel((uint32_t)(cx + dx), (uint32_t)(cy + dy), color);
}

void fb_draw_circle(int cx, int cy, int r, uint32_t color)
{
    int x = 0, y = r, d = 3 - 2 * r;
    while (x <= y) {
        fb_put_pixel((uint32_t)(cx + x), (uint32_t)(cy + y), color);
        fb_put_pixel((uint32_t)(cx - x), (uint32_t)(cy + y), color);
        fb_put_pixel((uint32_t)(cx + x), (uint32_t)(cy - y), color);
        fb_put_pixel((uint32_t)(cx - x), (uint32_t)(cy - y), color);
        fb_put_pixel((uint32_t)(cx + y), (uint32_t)(cy + x), color);
        fb_put_pixel((uint32_t)(cx - y), (uint32_t)(cy + x), color);
        fb_put_pixel((uint32_t)(cx + y), (uint32_t)(cy - x), color);
        fb_put_pixel((uint32_t)(cx - y), (uint32_t)(cy - x), color);
        if (d < 0) d += 4 * x + 6; else { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

/*  drop shadow  */

/*
 * Soft drop shadow under a rounded-rect window.
 * Call this BEFORE drawing the window itself.
 * layers=3 gives a nice progressive fade. Each layer is 40% more transparent.
 *
 * shadow_color: use 0x000000 for black.
 * base_alpha:   0–255, suggest 80–100 for dark desktop.
 */
void fb_draw_shadow(int x, int y, int w, int h, int r,
                    uint32_t shadow_color, uint8_t base_alpha)
{
    /* 4 shadow layers, each offset by 2px and more transparent */
    for (int i = 3; i >= 0; i--) {
        int    offset = (4 - i) * 2;
        uint8_t alpha = (uint8_t)(base_alpha >> (3 - i)); /* divide by 2 each step */

        /* corners */
        for (int cy = 0; cy <= r; cy++) {
            for (int cx = 0; cx <= r; cx++) {
                if (cx * cx + cy * cy <= r * r) {
                    fb_blend_pixel((uint32_t)(x + offset + r - cx),          (uint32_t)(y + offset + r - cy),          shadow_color, alpha);
                    fb_blend_pixel((uint32_t)(x + offset + w - r - 1 + cx),  (uint32_t)(y + offset + r - cy),          shadow_color, alpha);
                    fb_blend_pixel((uint32_t)(x + offset + r - cx),          (uint32_t)(y + offset + h - r - 1 + cy),  shadow_color, alpha);
                    fb_blend_pixel((uint32_t)(x + offset + w - r - 1 + cx),  (uint32_t)(y + offset + h - r - 1 + cy),  shadow_color, alpha);
                }
            }
        }
        /* fill inner strips with blended pixels */
        for (int row = y + offset + r; row < y + offset + h - r; row++) {
            for (int col = x + offset; col < x + offset + w; col++)
                fb_blend_pixel((uint32_t)col, (uint32_t)row, shadow_color, alpha);
        }
        for (int row = y + offset; row < y + offset + r; row++) {
            for (int col = x + offset + r; col < x + offset + w - r; col++)
                fb_blend_pixel((uint32_t)col, (uint32_t)row, shadow_color, alpha);
        }
        for (int row = y + offset + h - r; row < y + offset + h; row++) {
            for (int col = x + offset + r; col < x + offset + w - r; col++)
                fb_blend_pixel((uint32_t)col, (uint32_t)row, shadow_color, alpha);
        }
    }
}

/*  window chrome  */

/*
 * Draw a complete window frame with:
 *   - drop shadow
 *   - rounded body (bg_color)
 *   - title bar (title_color) with separator
 *   - left accent stripe (accent)
 *   - traffic-light close/min/max buttons
 *   - title text
 *
 * Returns the Y position where the content area starts.
 */
int fb_draw_window(int x, int y, int w, int h,
                   const char *title,
                   uint32_t bg_color,
                   uint32_t title_color,
                   uint32_t accent,
                   uint32_t text_color,
                   uint32_t border_color)
{
    int radius   = 6;
    int title_h  = 28;

    /* 1. shadow */
    fb_draw_shadow(x, y, w, h, radius, 0x000000, 90);

    /* 2. full window body */
    fb_fill_rounded_rect(x, y, w, h, radius, bg_color);

    /* 3. title bar — fill over top portion then square bottom corners */
    fb_fill_rounded_rect(x, y, w, title_h, radius, title_color);
    fb_fill_rect((uint32_t)x, (uint32_t)(y + radius),
                 (uint32_t)w, (uint32_t)(title_h - radius), title_color);

    /* 4. accent stripe */
    fb_fill_rounded_rect(x + 8, y + 7, 3, 14, 1, accent);

    /* 5. title text */
    extern void font_draw_str(uint32_t, uint32_t, const char *, uint32_t, uint32_t);
    font_draw_str((uint32_t)(x + 18), (uint32_t)(y + 8), title, text_color, title_color);

    /* 6. traffic-light buttons */
    fb_fill_circle(x + w - 40, y + title_h / 2, 5, 0xF59E0B); /* min  - amber */
    fb_fill_circle(x + w - 26, y + title_h / 2, 5, 0x10B981); /* max  - green */
    fb_fill_circle(x + w - 12, y + title_h / 2, 5, 0xEF4444); /* close- red   */

    /* 7. border */
    fb_draw_rounded_rect(x, y, w, h, radius, border_color);

    /* 8. title/body divider */
    fb_draw_hline(x, y + title_h, w, border_color);

    return y + title_h + 1; /* content area starts here */
}

/*  sidebar icon button  */

/*
 * Draw a sidebar icon button at position (sidebar_x, y).
 * icon_char: single ASCII character used as icon glyph.
 * active: 1 = highlighted.
 */
void fb_draw_sidebar_btn(int sidebar_x, int y, char icon_char,
                         const char *label,
                         uint32_t icon_color,
                         uint32_t bg_color,
                         uint32_t hover_color,
                         int active)
{
    extern void font_draw_char(uint32_t, uint32_t, char, uint32_t, uint32_t);
    extern void font_draw_str (uint32_t, uint32_t, const char *, uint32_t, uint32_t);

    int btn_w = 48, btn_h = 48;

    /* active/hover background pill */
    if (active)
        fb_fill_rounded_rect(sidebar_x + 4, y + 4, btn_w - 8, btn_h - 8, 7, hover_color);

    /* icon tinted background */
    fb_fill_rounded_rect(sidebar_x + 13, y + 8, 22, 22, 5,
                         fb_color_lighten(0x000000, 15)); /* subtle dark tint */

    /* icon glyph (centered in 22×22 box, glyph is 8×16) */
    font_draw_char((uint32_t)(sidebar_x + 13 + 7),
                   (uint32_t)(y + 8 + 3),
                   icon_char,
                   icon_color,
                   fb_color_lighten(0x000000, 15));

    /* label */
    font_draw_str((uint32_t)(sidebar_x + 8), (uint32_t)(y + 34),
                  label, 0x8B949E, bg_color);
}

/*  taskbar pill  */

/*
 * Draw a taskbar window pill.
 * active: 1 = focused window.
 * Returns x position after this pill (for chaining).
 */
int fb_draw_taskbar_pill(int x, int y, const char *label,
                          uint32_t accent,
                          uint32_t bg_color,
                          int active)
{
    extern void font_draw_str(uint32_t, uint32_t, const char *, uint32_t, uint32_t);

    /* measure label length roughly */
    int len = 0; while (label[len]) len++;
    int pill_w = len * 8 + 20; /* 8px per char + padding */
    int pill_h = 18;

    uint32_t fill   = active ? fb_color_lighten(0x000000, 18) : fb_color_lighten(0x000000, 12);
    uint32_t border = active ? accent : 0x30363D;
    uint32_t text   = active ? accent : 0x8B949E;

    fb_fill_rounded_rect(x, y + 1, pill_w, pill_h, 9, fill);
    fb_draw_rounded_rect(x, y + 1, pill_w, pill_h, 9, border);

    /* dot */
    fb_fill_circle(x + 8, y + 1 + pill_h / 2, 2, text);

    /* text */
    font_draw_str((uint32_t)(x + 14), (uint32_t)(y + 3), label, text, fill);

    return x + pill_w + 6;
}

/*  Bresenham line (kept for diagonal use)  */

void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx =  (x1 > x0 ? x1 - x0 : x0 - x1);
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        fb_put_pixel((uint32_t)x0, (uint32_t)y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}


/*  Double Buffer  */





int fb_back_init(void)
{
    uint64_t size = (uint64_t)g_fb.pitch * g_fb.height;
    g_back_buf = (uint8_t *)kmalloc(size);
    if (!g_back_buf) return -1;
    
    for (uint64_t i = 0; i < size / 4; i++)
        g_back_buf[i] = 0;
    return 0;
}

void fb_flip(void)
{
    if (!g_back_buf || !g_fb.active) return;
    uint64_t size = (uint64_t)g_fb.pitch * g_fb.height;
    __builtin_memcpy((void *)(uintptr_t)g_fb.addr, g_back_buf, size);
}

/*
 * Blit a whole ARGB32 buffer (top byte = alpha) into the framebuffer
 * at (dst_x, dst_y), width x height, in ONE kernel-space loop.
 *
 * This exists because the previous approach — the caller looping over
 * every pixel of a window and issuing one fb_put_pixel/fb_blend_pixel
 * *syscall* per pixel — meant a single 400x300 window redraw cost
 * ~120,000 syscalls (context switches) every frame. That overhead is
 * what was making window drag/redraw look laggy and "flickery": doing
 * the same loop here costs one syscall total, with the per-pixel work
 * done in kernel space where it's cheap.
 */
/*
 * NOTE on "zero-copy": a compositor is fundamentally combining
 * multiple surfaces (windows + wallpaper + cursor) into one image, so
 * there's no way to hand the display hardware a client's buffer
 * directly the way a fullscreen, undecorated single-surface app could
 * — that only becomes possible with hardware overlay planes / a real
 * GPU, which this kernel doesn't have a driver for. What we CAN do
 * cheaply is avoid redundant work on the one copy that does have to
 * happen: resolve the destination pointer once per row instead of
 * re-deriving it (and re-checking bounds/back-buffer selection) on
 * every single pixel via fb_put_pixel, and memcpy contiguous runs of
 * fully-opaque pixels — the common case for most window content —
 * directly instead of storing them one at a time.
 */
void fb_blit(int32_t dst_x, int32_t dst_y, uint32_t w, uint32_t h,
             const uint32_t *src, uint32_t bg_color)
{
    if (!g_fb.active || !src) return;

    uint8_t br  = (uint8_t)(bg_color >> 16);
    uint8_t bgc = (uint8_t)(bg_color >> 8);
    uint8_t bb  = (uint8_t)(bg_color);

    uint32_t stride_px = g_fb.pitch / 4; /* dest pixels per row */

    for (uint32_t row = 0; row < h; row++) {
        int32_t py = dst_y + (int32_t)row;
        if (py < 0 || (uint32_t)py >= g_fb.height) continue;

        /* Resolve the destination row base pointer ONCE (not per pixel) */
        uint32_t *dst_row = g_back_buf
            ? (uint32_t *)g_back_buf + (uint64_t)py * stride_px
            : (uint32_t *)(uintptr_t)(g_fb.addr + (uint64_t)py * g_fb.pitch);

        const uint32_t *src_row = src + (uint64_t)row * w;

        uint32_t col = 0;
        while (col < w) {
            int32_t px = dst_x + (int32_t)col;
            if (px < 0 || (uint32_t)px >= g_fb.width) { col++; continue; }

            uint32_t pix = src_row[col];
            uint8_t  a   = (uint8_t)(pix >> 24);

            if (a == 0) { col++; continue; }

            if (a == 0xFF) {
                /* Find how far this fully-opaque, in-bounds run goes
                 * and copy it in one shot instead of pixel-by-pixel. */
                uint32_t run_start = col;
                while (col < w) {
                    int32_t rp = dst_x + (int32_t)col;
                    if (rp < 0 || (uint32_t)rp >= g_fb.width) break;
                    if ((uint8_t)(src_row[col] >> 24) != 0xFF) break;
                    col++;
                }
                uint32_t run_len = col - run_start;
                __builtin_memcpy(&dst_row[dst_x + (int32_t)run_start],
                                  &src_row[run_start],
                                  (uint64_t)run_len * 4);
                continue;
            }

            /* Semi-transparent pixel: blend against bg_color */
            uint32_t color = pix & 0x00FFFFFF;
            uint8_t sr = (uint8_t)(color >> 16);
            uint8_t sg = (uint8_t)(color >> 8);
            uint8_t sb = (uint8_t)(color);
            uint8_t r  = (uint8_t)((sr * a + br  * (255 - a)) / 255);
            uint8_t g  = (uint8_t)((sg * a + bgc * (255 - a)) / 255);
            uint8_t b  = (uint8_t)((sb * a + bb  * (255 - a)) / 255);
            dst_row[px] = fb_rgb(r, g, b);
            col++;
        }
    }
}