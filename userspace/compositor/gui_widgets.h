/*
 * gui_widgets.h -- Exploidus GUI Widget Toolkit
 *
 * Every Exploidus GUI app so far (gui_demo.c, terminal.c) drew
 * directly into its own SHM pixel buffer with hand-rolled rect/text
 * calls, reimplementing the same button/label/textbox logic from
 * scratch each time. This is the shared, reusable version: apps
 * build their UI out of these instead.
 *
 * Uses the same dark theme palette as the compositor itself
 * (COL_SURFACE, COL_ACCENT_PUR, etc -- see compositor.c) so widgets
 * look consistent with window chrome, dock, and menubar.
 *
 * Draws into a plain uint32_t ARGB32 buffer (your window's SHM
 * buffer) -- no compositor/syscall involvement, same as gui_font.h.
 * Mouse/keyboard input comes from the compositor's IPC messages
 * (mouse_btn_msg_t, key_msg_t); this header only handles hit-testing
 * and drawing, the caller's own event loop feeds it input.
 *
 * Usage:
 *   gui_button_t btn = { .x=10, .y=10, .w=100, .h=32, .label="OK" };
 *   gui_button_draw(buf, WIN_W, WIN_H, &btn);
 *   // on IPC_MSG_MOUSE_BTN with pressed=1:
 *   if (gui_button_hit(&btn, mx, my)) { ... clicked ... }
 *
 *   gui_textbox_t tb = { .x=10, .y=50, .w=200, .h=28 };
 *   gui_textbox_draw(buf, WIN_W, WIN_H, &tb, focused);
 *   // on IPC_MSG_KEY_DOWN, if this textbox is focused:
 *   gui_textbox_input(&tb, key_ascii);
 */

#pragma once
#ifdef __EXPLOIDUS_USERSPACE__
#  include "../libc/stdint.h"
#else
#  include <stdint.h>
#endif
#include "gui_font.h"

/*  theme -- matches compositor.c's palette  */

#define GUI_COL_SURFACE    0xFF161B22u
#define GUI_COL_ELEVATED   0xFF1F2937u
#define GUI_COL_BORDER     0xFF30363Du
#define GUI_COL_TEXT_PRI   0xFFE6EDF3u
#define GUI_COL_TEXT_SEC   0xFF8B949Eu
#define GUI_COL_ACCENT_PUR 0xFFA78BFAu
#define GUI_COL_ACCENT_BLU 0xFF58A6FFu
#define GUI_COL_ACCENT_GRN 0xFF10B981u

/*  shared draw primitives  */

static inline void gui_fill_rect(uint32_t *buf, int buf_w, int buf_h,
                                  int x, int y, int w, int h, uint32_t col)
{
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= buf_h) continue;
        for (int c = x; c < x + w; c++) {
            if (c < 0 || c >= buf_w) continue;
            buf[row * buf_w + c] = col;
        }
    }
}

static inline void gui_draw_border(uint32_t *buf, int buf_w, int buf_h,
                                    int x, int y, int w, int h, uint32_t col)
{
    gui_fill_rect(buf, buf_w, buf_h, x,         y,         w, 1, col);
    gui_fill_rect(buf, buf_w, buf_h, x,         y + h - 1, w, 1, col);
    gui_fill_rect(buf, buf_w, buf_h, x,         y,         1, h, col);
    gui_fill_rect(buf, buf_w, buf_h, x + w - 1, y,         1, h, col);
}

/* Centers text horizontally within [x, x+w). */
static inline void gui_text_centered(uint32_t *buf, int buf_w, int buf_h,
                                      int x, int y, int w,
                                      const char *s, uint32_t fg)
{
    int len = 0;
    while (s[len]) len++;
    int tw = len * GUI_FONT_W;
    int tx = x + (w - tw) / 2;
    if (tx < x) tx = x;
    gui_font_str(buf, buf_w, buf_h, tx, y, s, fg, 0);
}

/*  button  */

typedef enum {
    GUI_BTN_NORMAL = 0,
    GUI_BTN_HOVER,
    GUI_BTN_PRESSED,
} gui_btn_state_t;

typedef struct {
    int             x, y, w, h;
    const char     *label;
    gui_btn_state_t state;
    uint32_t        accent;   /* 0 = use default purple */
} gui_button_t;

static inline void gui_button_draw(uint32_t *buf, int buf_w, int buf_h,
                                    const gui_button_t *b)
{
    uint32_t accent = b->accent ? b->accent : GUI_COL_ACCENT_PUR;
    uint32_t fill;
    switch (b->state) {
        case GUI_BTN_PRESSED: fill = accent;           break;
        case GUI_BTN_HOVER:   fill = GUI_COL_ELEVATED;  break;
        default:              fill = GUI_COL_SURFACE;   break;
    }
    gui_fill_rect(buf, buf_w, buf_h, b->x, b->y, b->w, b->h, fill);
    gui_draw_border(buf, buf_w, buf_h, b->x, b->y, b->w, b->h,
                     b->state == GUI_BTN_PRESSED ? accent : GUI_COL_BORDER);

    uint32_t text_col = (b->state == GUI_BTN_PRESSED) ? 0xFF0D1117u : GUI_COL_TEXT_PRI;
    int ty = b->y + (b->h - GUI_FONT_H) / 2;
    gui_text_centered(buf, buf_w, buf_h, b->x, ty, b->w, b->label, text_col);
}

/* True if (mx,my) -- window-relative, same coordinates the
 * compositor's mouse_btn_msg_t uses -- falls inside the button. */
static inline int gui_button_hit(const gui_button_t *b, int mx, int my)
{
    return mx >= b->x && mx < b->x + b->w &&
           my >= b->y && my < b->y + b->h;
}

/*  label  */

typedef struct {
    int         x, y;
    const char *text;
    uint32_t    color;   /* 0 = default primary text color */
} gui_label_t;

static inline void gui_label_draw(uint32_t *buf, int buf_w, int buf_h,
                                   const gui_label_t *l)
{
    gui_font_str(buf, buf_w, buf_h, l->x, l->y, l->text,
                 l->color ? l->color : GUI_COL_TEXT_PRI, 0);
}

/*  checkbox  */

typedef struct {
    int         x, y;
    const char *label;
    int         checked;
} gui_checkbox_t;

#define GUI_CHECKBOX_BOX 16

static inline void gui_checkbox_draw(uint32_t *buf, int buf_w, int buf_h,
                                      const gui_checkbox_t *c)
{
    gui_fill_rect(buf, buf_w, buf_h, c->x, c->y, GUI_CHECKBOX_BOX, GUI_CHECKBOX_BOX,
                  c->checked ? GUI_COL_ACCENT_PUR : GUI_COL_SURFACE);
    gui_draw_border(buf, buf_w, buf_h, c->x, c->y, GUI_CHECKBOX_BOX, GUI_CHECKBOX_BOX,
                     GUI_COL_BORDER);
    if (c->checked) {
        /* Simple checkmark: two short diagonal strokes */
        for (int i = 0; i < 5; i++) {
            gui_fill_rect(buf, buf_w, buf_h, c->x + 3 + i, c->y + 7 + i, 2, 2, 0xFF0D1117u);
            gui_fill_rect(buf, buf_w, buf_h, c->x + 7 + i, c->y + 11 - i, 2, 2, 0xFF0D1117u);
        }
    }
    if (c->label && c->label[0]) {
        gui_font_str(buf, buf_w, buf_h, c->x + GUI_CHECKBOX_BOX + 8,
                     c->y + (GUI_CHECKBOX_BOX - GUI_FONT_H) / 2,
                     c->label, GUI_COL_TEXT_PRI, 0);
    }
}

static inline int gui_checkbox_hit(const gui_checkbox_t *c, int mx, int my)
{
    /* Hit area covers the box plus the label, not just the box. */
    int len = 0;
    if (c->label) while (c->label[len]) len++;
    int w = GUI_CHECKBOX_BOX + (len ? 8 + len * GUI_FONT_W : 0);
    return mx >= c->x && mx < c->x + w &&
           my >= c->y && my < c->y + GUI_CHECKBOX_BOX;
}

/*  textbox (single-line input field)  */

#define GUI_TEXTBOX_MAX 255

typedef struct {
    int      x, y, w, h;
    char     text[GUI_TEXTBOX_MAX + 1];
    int      len;
    int      cursor_visible;  /* caller toggles this for blink */
} gui_textbox_t;

static inline void gui_textbox_draw(uint32_t *buf, int buf_w, int buf_h,
                                     const gui_textbox_t *t, int focused)
{
    gui_fill_rect(buf, buf_w, buf_h, t->x, t->y, t->w, t->h, GUI_COL_SURFACE);
    gui_draw_border(buf, buf_w, buf_h, t->x, t->y, t->w, t->h,
                     focused ? GUI_COL_ACCENT_BLU : GUI_COL_BORDER);

    int tx = t->x + 6;
    int ty = t->y + (t->h - GUI_FONT_H) / 2;
    gui_font_str(buf, buf_w, buf_h, tx, ty, t->text, GUI_COL_TEXT_PRI, 0);

    if (focused && t->cursor_visible) {
        int cx = tx + t->len * GUI_FONT_W;
        gui_fill_rect(buf, buf_w, buf_h, cx, ty, 2, GUI_FONT_H, GUI_COL_ACCENT_BLU);
    }
}

/*
 * gui_textbox_input -- feed one key's ASCII value in. Handles
 * printable characters and backspace; the caller's own event loop
 * decides when Enter/Tab should move focus elsewhere (that's app
 * logic, not something this generic widget should assume).
 * Returns 1 if the text actually changed, 0 otherwise (so the
 * caller knows whether a redraw is needed).
 */
static inline int gui_textbox_input(gui_textbox_t *t, char c)
{
    if (c == '\b' || c == 127) {
        if (t->len > 0) {
            t->len--;
            t->text[t->len] = 0;
            return 1;
        }
        return 0;
    }
    if (c >= 32 && c < 127 && t->len < GUI_TEXTBOX_MAX) {
        t->text[t->len++] = c;
        t->text[t->len] = 0;
        return 1;
    }
    return 0;
}

static inline int gui_textbox_hit(const gui_textbox_t *t, int mx, int my)
{
    return mx >= t->x && mx < t->x + t->w &&
           my >= t->y && my < t->y + t->h;
}

/*  scrollbar (vertical)  */

typedef struct {
    int x, y, h;         /* track position and length */
    int content_h;       /* total scrollable content height, in pixels */
    int view_h;          /* visible viewport height, in pixels */
    int offset;           /* current scroll offset, 0..(content_h-view_h) */
    int dragging;         /* set by the caller while the thumb is held */
    int drag_grab_offset;  /* pixel offset from thumb top to where the drag grabbed it */
} gui_scrollbar_t;

#define GUI_SCROLLBAR_W    10
#define GUI_SCROLLBAR_MINT 20   /* minimum thumb length, so it stays grabbable */

/* Computes the thumb's on-screen y and height for the current offset. */
static inline void gui_scrollbar_thumb(const gui_scrollbar_t *sb, int *thumb_y, int *thumb_h)
{
    if (sb->content_h <= sb->view_h) { *thumb_y = sb->y; *thumb_h = sb->h; return; }
    int th = sb->h * sb->view_h / sb->content_h;
    if (th < GUI_SCROLLBAR_MINT) th = GUI_SCROLLBAR_MINT;
    if (th > sb->h) th = sb->h;
    int max_offset = sb->content_h - sb->view_h;
    int track_travel = sb->h - th;
    int ty = sb->y + (max_offset > 0 ? sb->offset * track_travel / max_offset : 0);
    *thumb_y = ty;
    *thumb_h = th;
}

static inline void gui_scrollbar_draw(uint32_t *buf, int buf_w, int buf_h,
                                       const gui_scrollbar_t *sb)
{
    gui_fill_rect(buf, buf_w, buf_h, sb->x, sb->y, GUI_SCROLLBAR_W, sb->h, GUI_COL_SURFACE);
    if (sb->content_h <= sb->view_h) return;  /* nothing to scroll, empty track only */
    int ty, th;
    gui_scrollbar_thumb(sb, &ty, &th);
    gui_fill_rect(buf, buf_w, buf_h, sb->x + 2, ty, GUI_SCROLLBAR_W - 4, th,
                  sb->dragging ? GUI_COL_ACCENT_PUR : GUI_COL_BORDER);
}

static inline int gui_scrollbar_hit(const gui_scrollbar_t *sb, int mx, int my)
{
    return mx >= sb->x && mx < sb->x + GUI_SCROLLBAR_W &&
           my >= sb->y && my < sb->y + sb->h;
}

/*
 * gui_scrollbar_drag -- called on mouse move while sb->dragging is
 * set (the caller sets that on mouse-down over the thumb, clears it
 * on mouse-up). Updates sb->offset from the new mouse y. Returns 1 if
 * the offset actually changed (redraw needed), 0 otherwise.
 */
static inline int gui_scrollbar_drag(gui_scrollbar_t *sb, int my)
{
    if (sb->content_h <= sb->view_h) return 0;
    int ty, th;
    gui_scrollbar_thumb(sb, &ty, &th);
    int max_offset    = sb->content_h - sb->view_h;
    int track_travel  = sb->h - th;
    int new_ty        = my - sb->drag_grab_offset - sb->y;
    if (new_ty < 0) new_ty = 0;
    if (new_ty > track_travel) new_ty = track_travel;
    int new_offset = track_travel > 0 ? new_ty * max_offset / track_travel : 0;
    if (new_offset == sb->offset) return 0;
    sb->offset = new_offset;
    return 1;
}