/*
 * exui.h — Exploidus Lightweight Cyber-Themed GUI Widget Toolkit (Phase 5.3)
 *
 * Reusable, high-performance widget toolkit for native Exploidus desktop apps.
 * Features:
 *   - Dark cyan (0x00E5FF) & neon green (0x00FF66) cybersecurity aesthetic.
 *   - Widgets: Window, Button, TextBox, ListView, Panel, Menu, Canvas, Checkbox.
 *   - Direct 32-bpp ARGB pixel buffer rendering with zero dynamic heap dependencies.
 */

#pragma once

#ifdef __EXPLOIDUS_USERSPACE__
#  include "../../libc/stdint.h"
#  include "../../libc/string.h"
#else
#  include <stdint.h>
#  include <string.h>
#endif

#include "../../compositor/gui_font.h"

/*  Cybersecurity Theme Palette  */
#define EXUI_COL_BG           0xFF0A0F14u   /* Deep obsidian background */
#define EXUI_COL_SURFACE      0xFF121B24u   /* Elevated window surface */
#define EXUI_COL_ELEVATED     0xFF1A2634u   /* Cards, panels, headers */
#define EXUI_COL_BORDER       0xFF22384Du   /* Muted structural border */
#define EXUI_COL_BORDER_GLOW  0xFF00E5FFu   /* Active neon cyan border glow */
#define EXUI_COL_CYAN         0xFF00E5FFu   /* Primary neon cyan accent */
#define EXUI_COL_GREEN        0xFF00FF66u   /* Secondary neon green (security/status) */
#define EXUI_COL_AMBER        0xFFFFB300u   /* Warning status */
#define EXUI_COL_RED          0xFFFF3366u   /* Alert / error / close */
#define EXUI_COL_TEXT_PRI     0xFFE0F7FAu   /* Crisp ice-white primary text */
#define EXUI_COL_TEXT_SEC     0xFF78909Cu   /* Slate cyan secondary text */
#define EXUI_COL_TEXT_MUTED   0xFF455A64u   /* Dim text */

/*  Basic 2D Drawing Primitives  */

static inline void exui_fill_rect(uint32_t *buf, int buf_w, int buf_h,
                                  int x, int y, int w, int h, uint32_t col)
{
    for (int r = y; r < y + h; r++) {
        if (r < 0 || r >= buf_h) continue;
        for (int c = x; c < x + w; c++) {
            if (c < 0 || c >= buf_w) continue;
            buf[r * buf_w + c] = col;
        }
    }
}

static inline void exui_draw_border(uint32_t *buf, int buf_w, int buf_h,
                                    int x, int y, int w, int h, uint32_t col)
{
    exui_fill_rect(buf, buf_w, buf_h, x,         y,         w, 1, col);
    exui_fill_rect(buf, buf_w, buf_h, x,         y + h - 1, w, 1, col);
    exui_fill_rect(buf, buf_w, buf_h, x,         y,         1, h, col);
    exui_fill_rect(buf, buf_w, buf_h, x + w - 1, y,         1, h, col);
}

static inline void exui_draw_str(uint32_t *buf, int buf_w, int buf_h,
                                 int x, int y, const char *s,
                                 uint32_t fg, uint32_t bg)
{
    gui_font_str(buf, buf_w, buf_h, x, y, s, fg, bg);
}

static inline void exui_draw_str_trans(uint32_t *buf, int buf_w, int buf_h,
                                       int x, int y, const char *s,
                                       uint32_t fg)
{
    gui_font_str(buf, buf_w, buf_h, x, y, s, fg, 0);
}

/*  Widget 1: Label  */

typedef struct {
    int         x, y;
    const char *text;
    uint32_t    color;
} exui_label_t;

static inline void exui_label_draw(uint32_t *buf, int buf_w, int buf_h,
                                   const exui_label_t *lbl)
{
    if (!lbl || !lbl->text) return;
    exui_draw_str_trans(buf, buf_w, buf_h, lbl->x, lbl->y, lbl->text, lbl->color);
}

/*  Widget 2: Button  */

typedef struct {
    int         x, y, w, h;
    const char *label;
    uint32_t    accent_col;
    uint8_t     pressed;
    uint8_t     hovered;
} exui_button_t;

static inline int exui_button_hit(const exui_button_t *btn, int mx, int my)
{
    return (mx >= btn->x && mx < btn->x + btn->w &&
            my >= btn->y && my < btn->y + btn->h);
}

static inline void exui_button_draw(uint32_t *buf, int buf_w, int buf_h,
                                    const exui_button_t *btn)
{
    uint32_t bg = btn->pressed ? EXUI_COL_ELEVATED :
                  (btn->hovered ? 0xFF223446u : EXUI_COL_SURFACE);
    uint32_t border = btn->hovered ? (btn->accent_col ? btn->accent_col : EXUI_COL_CYAN) : EXUI_COL_BORDER;
    uint32_t fg = btn->hovered ? EXUI_COL_TEXT_PRI : EXUI_COL_TEXT_SEC;

    exui_fill_rect(buf, buf_w, buf_h, btn->x, btn->y, btn->w, btn->h, bg);
    exui_draw_border(buf, buf_w, buf_h, btn->x, btn->y, btn->w, btn->h, border);

    /* Left indicator notch */
    exui_fill_rect(buf, buf_w, buf_h, btn->x, btn->y, 3, btn->h,
                   btn->accent_col ? btn->accent_col : EXUI_COL_CYAN);

    /* Center text label */
    int len = 0; while (btn->label && btn->label[len]) len++;
    int tw = len * GUI_FONT_W;
    int tx = btn->x + (btn->w - tw) / 2;
    int ty = btn->y + (btn->h - GUI_FONT_H) / 2;
    exui_draw_str_trans(buf, buf_w, buf_h, tx, ty, btn->label, fg);
}

/*  Widget 3: TextBox  */

#define EXUI_TEXTBOX_MAX 128

typedef struct {
    int      x, y, w, h;
    char     buf[EXUI_TEXTBOX_MAX];
    int      len;
    int      cursor_pos;
    uint8_t  focused;
} exui_textbox_t;

static inline void exui_textbox_draw(uint32_t *buf, int buf_w, int buf_h,
                                     const exui_textbox_t *tb)
{
    uint32_t border = tb->focused ? EXUI_COL_CYAN : EXUI_COL_BORDER;
    exui_fill_rect(buf, buf_w, buf_h, tb->x, tb->y, tb->w, tb->h, EXUI_COL_BG);
    exui_draw_border(buf, buf_w, buf_h, tb->x, tb->y, tb->w, tb->h, border);

    int tx = tb->x + 8;
    int ty = tb->y + (tb->h - GUI_FONT_H) / 2;
    exui_draw_str_trans(buf, buf_w, buf_h, tx, ty, tb->buf, EXUI_COL_TEXT_PRI);

    /* Blinking vertical cursor */
    if (tb->focused) {
        int cx = tx + tb->cursor_pos * GUI_FONT_W;
        if (cx < tb->x + tb->w - 4) {
            exui_fill_rect(buf, buf_w, buf_h, cx, ty - 1, 2, GUI_FONT_H + 2, EXUI_COL_CYAN);
        }
    }
}

static inline void exui_textbox_input(exui_textbox_t *tb, char c)
{
    if (c == '\b') {
        if (tb->cursor_pos > 0) {
            tb->cursor_pos--;
            tb->len--;
            tb->buf[tb->len] = 0;
        }
    } else if (c >= 32 && c <= 126 && tb->len < EXUI_TEXTBOX_MAX - 1) {
        tb->buf[tb->len++] = c;
        tb->buf[tb->len] = 0;
        tb->cursor_pos++;
    }
}

/*  Widget 4: ListView (Scrollable list of items)  */

#define EXUI_LISTVIEW_MAX_ITEMS 64

typedef struct {
    int         x, y, w, h;
    int         item_height;
    int         count;
    int         selected;
    int         scroll_offset;
    char        items[EXUI_LISTVIEW_MAX_ITEMS][64];
    uint32_t    item_colors[EXUI_LISTVIEW_MAX_ITEMS];
} exui_listview_t;

static inline void exui_listview_draw(uint32_t *buf, int buf_w, int buf_h,
                                      const exui_listview_t *lv)
{
    exui_fill_rect(buf, buf_w, buf_h, lv->x, lv->y, lv->w, lv->h, EXUI_COL_SURFACE);
    exui_draw_border(buf, buf_w, buf_h, lv->x, lv->y, lv->w, lv->h, EXUI_COL_BORDER);

    int visible_items = lv->h / lv->item_height;
    for (int i = 0; i < visible_items; i++) {
        int idx = i + lv->scroll_offset;
        if (idx >= lv->count) break;

        int iy = lv->y + i * lv->item_height;
        uint32_t item_bg = (idx == lv->selected) ? EXUI_COL_ELEVATED : EXUI_COL_SURFACE;
        exui_fill_rect(buf, buf_w, buf_h, lv->x + 1, iy + 1, lv->w - 2, lv->item_height - 2, item_bg);

        if (idx == lv->selected) {
            /* Cyan selection marker */
            exui_fill_rect(buf, buf_w, buf_h, lv->x + 2, iy + 2, 3, lv->item_height - 4, EXUI_COL_CYAN);
        }

        uint32_t text_col = lv->item_colors[idx] ? lv->item_colors[idx] :
                            (idx == lv->selected ? EXUI_COL_CYAN : EXUI_COL_TEXT_PRI);
        exui_draw_str_trans(buf, buf_w, buf_h, lv->x + 10, iy + (lv->item_height - GUI_FONT_H) / 2,
                           lv->items[idx], text_col);
    }
}

static inline int exui_listview_click(exui_listview_t *lv, int mx, int my)
{
    if (mx < lv->x || mx >= lv->x + lv->w || my < lv->y || my >= lv->y + lv->h)
        return -1;
    int rel_y = my - lv->y;
    int clicked_idx = (rel_y / lv->item_height) + lv->scroll_offset;
    if (clicked_idx < lv->count) {
        lv->selected = clicked_idx;
        return clicked_idx;
    }
    return -1;
}

/*  Widget 5: Panel / Group Box  */

typedef struct {
    int         x, y, w, h;
    const char *title;
    uint32_t    accent_col;
} exui_panel_t;

static inline void exui_panel_draw(uint32_t *buf, int buf_w, int buf_h,
                                   const exui_panel_t *p)
{
    exui_fill_rect(buf, buf_w, buf_h, p->x, p->y, p->w, p->h, EXUI_COL_SURFACE);
    exui_draw_border(buf, buf_w, buf_h, p->x, p->y, p->w, p->h, EXUI_COL_BORDER);

    /* Header strip */
    int hh = 24;
    exui_fill_rect(buf, buf_w, buf_h, p->x, p->y, p->w, hh, EXUI_COL_ELEVATED);
    exui_fill_rect(buf, buf_w, buf_h, p->x, p->y, 3, hh, p->accent_col ? p->accent_col : EXUI_COL_CYAN);
    exui_fill_rect(buf, buf_w, buf_h, p->x, p->y + hh, p->w, 1, EXUI_COL_BORDER);

    if (p->title) {
        exui_draw_str_trans(buf, buf_w, buf_h, p->x + 10, p->y + 4, p->title, EXUI_COL_TEXT_PRI);
    }
}
