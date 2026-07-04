/*
 * compositor.c — Exploidus Display Compositor (Phase 2)
 *
 * Runs as /bin/compositor, spawned by init before the shell.
 * Responsibilities:
 *   1. Maintain a window list (up to MAX_WINDOWS)
 *   2. On IPC_MSG_WIN_CREATE: map the app's SHM buffer
 *   3. On IPC_MSG_DAMAGE: mark window dirty
 *   4. Each frame: composite all windows onto the framebuffer
 *      back-buffer, then fb_flip()
 *   5. Route mouse/keyboard events to the focused window
 *   6. Draw macOS-style desktop: wallpaper, dock, title bars
 *
 * Pixel format: 0x00RRGGBB (32-bit, top byte ignored / alpha blend)
 */

/* __EXPLOIDUS_USERSPACE__ defined by -D flag in SHELL_CFLAGS */
#include "../libc/syscall.h"
#include "compositor.h"

/*  tunables  */
#define MAX_WINDOWS      16
#define FRAME_TICKS      2      /* repaint every N scheduler ticks (~50fps at 100Hz) */
#define TITLEBAR_H       28     /* px — compact title bar  */
#define DOCK_H           56     /* px — bottom dock height             */
#define DOCK_ICON_SZ     40     /* dock icon size                      */
#define SHADOW_BLUR      6      /* window drop shadow radius           */
#define CORNER_R         8      /* window corner radius                */

/*  desktop palette (matches fb.h COL_* defines)  */
#define COL_DESKTOP      0x0D1117
#define COL_SURFACE      0x161B22
#define COL_ELEVATED     0x1F2937
#define COL_BORDER       0x30363D
#define COL_TEXT_PRI     0xE6EDF3
#define COL_TEXT_SEC     0x8B949E
#define COL_ACCENT_PUR   0xA78BFA
#define COL_ACCENT_BLU   0x58A6FF
#define COL_ACCENT_GRN   0x10B981
#define COL_BTN_CLOSE    0xEF4444
#define COL_BTN_MIN      0xF59E0B
#define COL_BTN_MAX      0x10B981

/*  helpers  */
static void _puts(const char *s) { write(1, s, strlen(s)); }
static void _putc(char c)        { write(1, &c, 1); }

static void _print_uint(uint64_t n)
{
    if (!n) { _putc('0'); return; }
    char b[20]; int i = 0;
    while (n) { b[i++] = '0' + (int)(n % 10); n /= 10; }
    while (i--) _putc(b[i]);
}

static void _println(const char *s) { _puts(s); _putc('\n'); }

static int _strlen(const char *s) { int n=0; while(*s++)n++; return n; }

static void _memset(void *dst, uint8_t v, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = v;
}

/*  window descriptor  */
typedef struct {
    uint32_t  shm_id;
    uint32_t *pixels;          /* mapped VA of app's SHM buffer          */
    int32_t   x, y;            /* screen position (of client area)       */
    uint32_t  w, h;            /* client area size in pixels             */
    uint32_t  flags;
    char      title[48];
    uint32_t  owner_pid;
    uint8_t   dirty;           /* needs repaint                          */
    uint8_t   valid;
} window_t;

/*  compositor state  */
static window_t g_windows[MAX_WINDOWS];
static int      g_win_count  = 0;
static int      g_focused    = -1;  /* index of focused window          */

/*  cascade position for new windows  */
static int32_t  g_cascade_x  = 40;
static int32_t  g_cascade_y  = 50;

/*  drag state  */
static int      g_drag_win   = -1;  /* window index being dragged, -1=none */
static int32_t  g_drag_off_x = 0;  /* mouse offset from window origin   */
static int32_t  g_drag_off_y = 0;

static uint32_t g_screen_w   = 0;
static uint32_t g_screen_h   = 0;
static uint32_t g_fb_active  = 0;

/*  window management  */

static window_t *find_win(uint32_t shm_id)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_windows[i].valid && g_windows[i].shm_id == shm_id)
            return &g_windows[i];
    return (window_t *)0;
}

static window_t *alloc_win(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!g_windows[i].valid) return &g_windows[i];
    return (window_t *)0;
}

static void free_win(window_t *w)
{
    if (!w) return;
    if (w->pixels) {
        shm_unmap(w->pixels, w->w * w->h * 4);
        w->pixels = (uint32_t *)0;
    }
    _memset(w, 0, sizeof(window_t));
}

/*  inline draw helpers (direct to SYS_FB_* syscalls)  */

static inline uint32_t blend(uint32_t bg, uint32_t fg, uint8_t a)
{
    /* Simple alpha blend: result = fg*a/255 + bg*(255-a)/255 */
    uint8_t rb = (uint8_t)(bg >> 16);
    uint8_t gb = (uint8_t)(bg >>  8);
    uint8_t bb = (uint8_t)(bg);
    uint8_t rf = (uint8_t)(fg >> 16);
    uint8_t gf = (uint8_t)(fg >>  8);
    uint8_t bf = (uint8_t)(fg);
    uint8_t ro = (uint8_t)((rf * a + rb * (255 - a)) / 255);
    uint8_t go = (uint8_t)((gf * a + gb * (255 - a)) / 255);
    uint8_t bo = (uint8_t)((bf * a + bb * (255 - a)) / 255);
    return ((uint32_t)ro << 16) | ((uint32_t)go << 8) | bo;
}

/* Draw a filled rectangle via syscall */
static void draw_rect(int x, int y, int w, int h, uint32_t col)
{
    fb_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, col);
}

/* Draw a single pixel */
static void draw_pixel(int x, int y, uint32_t col)
{
    fb_pixel((uint32_t)x, (uint32_t)y, col);
}

/* Draw a string */
static void draw_str(int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    fb_str((uint32_t)x, (uint32_t)y, s, fg, bg);
}

/* Draw rounded rect */
static void draw_rrect(int x, int y, int w, int h, int r, uint32_t col)
{
    fb_rrect(x, y, w, h, r, col);
}

/* Draw circle */
static void draw_circle(int cx, int cy, int r, uint32_t col)
{
    fb_circle(cx, cy, r, col);
}

/*  desktop background  */

static void draw_wallpaper(void)
{
    /* Dark gradient-ish wallpaper: solid base + subtle top fade */
    fb_clear(COL_DESKTOP);

    /* Subtle purple glow in top-left corner */
    uint32_t gw = g_screen_w / 3;
    uint32_t gh = g_screen_h / 3;
    for (uint32_t r = 0; r < 120; r += 8) {
        uint8_t a = (uint8_t)(30 - r / 4);
        if (a == 0) break;
        draw_circle((int)(gw / 2), (int)(gh / 2), (int)r,
                    blend(COL_DESKTOP, COL_ACCENT_PUR, a));
    }

    /* Subtle blue glow bottom-right */
    for (uint32_t r = 0; r < 160; r += 8) {
        uint8_t a = (uint8_t)(25 - r / 7);
        if (a == 0) break;
        draw_circle((int)(g_screen_w - 120), (int)(g_screen_h - 80),
                    (int)r,
                    blend(COL_DESKTOP, COL_ACCENT_BLU, a));
    }
}

/* menu bar  */

static void draw_menubar(void)
{
    int mh = 22;
    /* Frosted-glass look: slightly lighter than desktop */
    draw_rect(0, 0, (int)g_screen_w, mh, 0x161B22);

    /* Apple-style logo placeholder */
    draw_str(10, 4, "Exploidus", COL_TEXT_PRI, 0x161B22);

    /* Focused window title in center */
    if (g_focused >= 0 && g_windows[g_focused].valid) {
        const char *t = g_windows[g_focused].title;
        int tw = _strlen(t) * 8;   /* 8px per char (bitmap font) */
        int tx = ((int)g_screen_w - tw) / 2;
        if (tx < 80) tx = 80;
        draw_str(tx, 4, t, COL_TEXT_PRI, 0x161B22);
    }

    /* Clock area placeholder (right side) */
    draw_str((int)g_screen_w - 60, 4, "Exploidus", COL_TEXT_SEC, 0x161B22);

    /* Bottom border */
    draw_rect(0, mh - 1, (int)g_screen_w, 1, COL_BORDER);
}

/* dock  */

/* Dock app definitions — label, color, binary path */
typedef struct {
    const char *label;
    uint32_t    col;
    const char *path;
} dock_item_t;

#define DOCK_ITEMS 3
static const dock_item_t g_dock_items[DOCK_ITEMS] = {
    { "T", COL_ACCENT_PUR, "/bin/terminal" },
    { "F", COL_ACCENT_GRN, "/bin/exploish" },
    { "S", COL_ACCENT_BLU, "/bin/gui_demo" },
};

/* dock geometry — computed once, used for hit testing */
static int g_dock_dx = 0, g_dock_dy = 0;
static int g_dock_dw = 200, g_dock_spacing = 0;

static void draw_dock(void)
{
    int dy = (int)g_screen_h - DOCK_H;
    int dw = 200;
    int dx = ((int)g_screen_w - dw) / 2;
    int spacing = dw / (DOCK_ITEMS + 1);

    /* Save geometry for hit testing */
    g_dock_dx = dx; g_dock_dy = dy;
    g_dock_dw = dw; g_dock_spacing = spacing;

    /* Dock background — frosted pill */
    draw_rrect(dx, dy + 6, dw, DOCK_H - 8, 10, 0x1F2937);
    draw_rrect(dx, dy + 6, dw, DOCK_H - 8, 10, 0x30363D);

    for (int i = 0; i < DOCK_ITEMS; i++) {
        int ix = dx + spacing * (i + 1) - DOCK_ICON_SZ / 2;
        int iy = dy + (DOCK_H - DOCK_ICON_SZ) / 2;
        draw_rrect(ix, iy, DOCK_ICON_SZ, DOCK_ICON_SZ, 8, g_dock_items[i].col);
        draw_str(ix + 14, iy + 12, g_dock_items[i].label,
                 COL_TEXT_PRI, g_dock_items[i].col);
    }
}

/* dock_hit — returns dock item index (0-based) if click is on a dock icon,
 * or -1 if no hit. */
static int dock_hit(int32_t mx, int32_t my)
{
    int dy = g_dock_dy;
    int iy = dy + (DOCK_H - DOCK_ICON_SZ) / 2;
    if (my < iy || my >= iy + DOCK_ICON_SZ) return -1;

    for (int i = 0; i < DOCK_ITEMS; i++) {
        int ix = g_dock_dx + g_dock_spacing * (i + 1) - DOCK_ICON_SZ / 2;
        if (mx >= ix && mx < ix + DOCK_ICON_SZ) return i;
    }
    return -1;
}

/*  window chrome (title bar + buttons)  */

static void draw_window_chrome(window_t *w)
{
    int wx = w->x;
    int wy = w->y - TITLEBAR_H;    /* title bar is above client area */
    int ww = (int)w->w;

    /* Shadow */
    for (int i = SHADOW_BLUR; i > 0; i--) {
        uint8_t a = (uint8_t)(12 * (SHADOW_BLUR - i + 1) / SHADOW_BLUR);
        draw_rrect(wx - i, wy - i,
                   ww + i * 2, (int)w->h + TITLEBAR_H + i * 2,
                   CORNER_R + i,
                   blend(COL_DESKTOP, 0x000000, a));
    }

    /* Title bar */
    draw_rrect(wx, wy, ww, TITLEBAR_H + CORNER_R, CORNER_R, COL_ELEVATED);
    /* Cover bottom-rounded part of title bar (client area border) */
    draw_rect(wx, wy + TITLEBAR_H - CORNER_R, ww, CORNER_R, COL_ELEVATED);

    /* Traffic-light buttons */
    int btn_y = wy + TITLEBAR_H / 2;
    draw_circle(wx + 14, btn_y, 5, COL_BTN_CLOSE);
    draw_circle(wx + 30, btn_y, 5, COL_BTN_MIN);
    draw_circle(wx + 46, btn_y, 5, COL_BTN_MAX);

    /* Title text — centered */
    int tw = _strlen(w->title) * 8;
    int tx = wx + (ww - tw) / 2;
    if (tx < wx + 60) tx = wx + 60;
    draw_str(tx, wy + (TITLEBAR_H - 8) / 2, w->title,
             COL_TEXT_PRI, COL_ELEVATED);

    /* Border around client area */
    draw_rect(wx, wy + TITLEBAR_H, 1, (int)w->h, COL_BORDER);        /* left  */
    draw_rect(wx + ww - 1, wy + TITLEBAR_H, 1, (int)w->h, COL_BORDER); /* right */
    draw_rect(wx, wy + TITLEBAR_H + (int)w->h, ww, 1, COL_BORDER);  /* bottom */
}

/*  blit app pixel buffer to framebuffer  */

static void blit_window(window_t *w)
{
    if (!w->pixels) return;

    uint32_t *src = w->pixels;
    int32_t   sx  = w->x;
    int32_t   sy  = w->y;
    uint32_t  sw  = w->w;
    uint32_t  sh  = w->h;

    /* Clip to screen */
    int32_t x0 = sx < 0 ? 0 : sx;
    int32_t y0 = sy < 0 ? 0 : sy;
    int32_t x1 = sx + (int32_t)sw;
    int32_t y1 = sy + (int32_t)sh;
    if (x1 > (int32_t)g_screen_w) x1 = (int32_t)g_screen_w;
    if (y1 > (int32_t)g_screen_h - DOCK_H) y1 = (int32_t)g_screen_h - DOCK_H;

    for (int32_t row = y0; row < y1; row++) {
        for (int32_t col = x0; col < x1; col++) {
            uint32_t pix = src[(row - sy) * sw + (col - sx)];
            /* Check alpha channel (top byte) */
            uint8_t a = (uint8_t)(pix >> 24);
            if (a == 0) continue;
            if (a == 0xFF) {
                draw_pixel(col, row, pix & 0x00FFFFFF);
            } else {
                /* Alpha blend with whatever was drawn already */
                draw_pixel(col, row, blend(COL_DESKTOP, pix & 0x00FFFFFF, a));
            }
        }
    }
}

/*  full composite frame  */

static void composite_frame(void)
{
    draw_wallpaper();
    draw_menubar();

    /* Draw windows back-to-front (painter's algorithm) */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_windows[i].valid) continue;
        if (i == g_focused) continue;
        if (g_windows[i].flags & WIN_FLAG_DECORATED)
            draw_window_chrome(&g_windows[i]);
        blit_window(&g_windows[i]);
    }
    /* Draw focused window on top */
    if (g_focused >= 0 && g_windows[g_focused].valid) {
        window_t *fw = &g_windows[g_focused];
        if (fw->flags & WIN_FLAG_DECORATED)
            draw_window_chrome(fw);
        blit_window(fw);
    }

    draw_dock();
    /* NOTE: NO fb_flip() here — caller draws cursor then flips once */
}

/*  IPC message handlers  */

static void handle_win_create(ipc_msg_t *msg)
{
    win_create_msg_t *p = (win_create_msg_t *)msg->data;
    _puts("[COMP] WIN_CREATE shm_id="); _print_uint(p->shm_id);
    _puts(" from PID="); _print_uint(msg->from_pid); _putc('\n');

    if (find_win(p->shm_id)) {
        _println("[COMP] window already registered");
        return;
    }

    window_t *w = alloc_win();
    if (!w) { _println("[COMP] window table full"); return; }

    w->shm_id    = p->shm_id;
    /* If app passes x=0,y=0 use cascade position, else use app's position */
    if (p->x == 0 && p->y == 0) {
        w->x = g_cascade_x;
        w->y = g_cascade_y + TITLEBAR_H;
        /* Advance cascade — diagonal step */
        g_cascade_x += 30;
        g_cascade_y += 30;
        /* Wrap cascade so windows don't go off screen */
        if (g_cascade_x > (int32_t)g_screen_w - 200) g_cascade_x = 40;
        if (g_cascade_y > (int32_t)g_screen_h - 200) g_cascade_y = 50;
    } else {
        w->x = p->x;
        w->y = p->y + TITLEBAR_H;
    }
    w->w         = p->w;
    w->h         = p->h;
    w->flags     = p->flags | WIN_FLAG_DECORATED;
    w->owner_pid = msg->from_pid;
    w->dirty     = 1;
    w->valid     = 1;

    /* Copy title */
    int ti = 0;
    while (p->title[ti] && ti < 47) { w->title[ti] = p->title[ti]; ti++; }
    w->title[ti] = 0;

    /* Map the app's SHM pixel buffer */
    w->pixels = (uint32_t *)shm_map(p->shm_id);
    if (!w->pixels) {
        _println("[COMP] shm_map failed");
        _memset(w, 0, sizeof(window_t));
        return;
    }

    /* New window gets focus */
    g_focused = (int)(w - g_windows);
    g_win_count++;
    _puts("[COMP] registered window '"); _puts(w->title); _println("'");
}

static void handle_win_destroy(ipc_msg_t *msg)
{
    win_destroy_msg_t *p = (win_destroy_msg_t *)msg->data;
    window_t *w = find_win(p->shm_id);
    if (!w) return;

    _puts("[COMP] WIN_DESTROY '"); _puts(w->title); _println("'");

    if (g_focused == (int)(w - g_windows)) {
        g_focused = -1;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (g_windows[i].valid && &g_windows[i] != w) {
                g_focused = i; break;
            }
        }
    }
    free_win(w);
    g_win_count--;
}

/* Close button: destroy by window index (compositor-side force close) */
static void handle_win_destroy_by_idx(int idx)
{
    if (idx < 0 || idx >= MAX_WINDOWS) return;
    window_t *w = &g_windows[idx];
    if (!w->valid) return;

    _puts("[COMP] CLOSE '"); _puts(w->title); _println("'");

    if (g_focused == idx) {
        g_focused = -1;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (g_windows[i].valid && i != idx) {
                g_focused = i; break;
            }
        }
    }
    if (g_drag_win == idx) g_drag_win = -1;
    free_win(w);
    g_win_count--;
}

static void handle_damage(ipc_msg_t *msg)
{
    win_damage_msg_t *p = (win_damage_msg_t *)msg->data;
    window_t *w = find_win(p->shm_id);
    if (w) w->dirty = 1;
}

static void handle_win_move(ipc_msg_t *msg)
{
    win_move_msg_t *p = (win_move_msg_t *)msg->data;
    window_t *w = find_win(p->shm_id);
    if (!w) return;
    w->x = p->new_x;
    w->y = p->new_y + TITLEBAR_H;
    w->dirty = 1;
}

/*  mouse event routing  */

static void route_mouse(int32_t mx, int32_t my, int32_t btn,
                        int32_t prev_btn)
{
    int left_down    = (btn & 1) && !(prev_btn & 1);   /* left just pressed  */
    int left_up      = !(btn & 1) && (prev_btn & 1);   /* left just released */
    int left_held    = (btn & 1);

    /*  Dragging: mouse button still held and we started a drag  */
    if (left_held && g_drag_win >= 0) {
        window_t *dw = &g_windows[g_drag_win];
        if (dw->valid) {
            dw->x = mx - g_drag_off_x;
            dw->y = my - g_drag_off_y;
            /* Clamp to screen */
            if (dw->x < 0) dw->x = 0;
            if (dw->y < TITLEBAR_H + 22) dw->y = TITLEBAR_H + 22;
            if (dw->x + (int32_t)dw->w > (int32_t)g_screen_w)
                dw->x = (int32_t)g_screen_w - (int32_t)dw->w;
            if (dw->y + (int32_t)dw->h > (int32_t)g_screen_h - DOCK_H)
                dw->y = (int32_t)g_screen_h - DOCK_H - (int32_t)dw->h;
        }
        return;
    }

    /*  Release drag  */
    if (left_up && g_drag_win >= 0) {
        g_drag_win = -1;
        return;
    }

    /*  Dock click  */
    if (left_down) {
        int ditem = dock_hit(mx, my);
        if (ditem >= 0) {
            /* Spawn the dock app */
            spawn(g_dock_items[ditem].path);
            return;
        }
    }

    /*  Find which window the mouse is over  */
    int hit = -1;
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        if (!g_windows[i].valid) continue;
        window_t *w = &g_windows[i];
        int wy = w->y - TITLEBAR_H;
        if (mx >= w->x && mx < w->x + (int32_t)w->w &&
            my >= wy    && my < w->y + (int32_t)w->h) {
            hit = i; break;
        }
    }

    if (hit < 0) return;

    /*  Left button pressed  */
    if (left_down) {
        g_focused = hit;
        window_t *hw = &g_windows[hit];
        int tby = hw->y - TITLEBAR_H;   /* top of title bar */
        int btn_y = tby + TITLEBAR_H / 2;

        /*  Close button (red circle at wx+14, btn_y)  */
        int dx_close = mx - (hw->x + 14);
        int dy_close = my - btn_y;
        if (dx_close*dx_close + dy_close*dy_close <= 25) {
            /* Send destroy signal to the window's owner */
            ipc_msg_t ev;
            ev.type = IPC_MSG_WIN_BLUR;  /* reuse as "close" hint */
            ev.len  = 0;
            ipc_send(hw->owner_pid, &ev);
            /* Force-remove from compositor side immediately */
            handle_win_destroy_by_idx(hit);
            return;
        }

        /*  Title bar drag area  */
        if (my >= tby && my < hw->y) {
            g_drag_win   = hit;
            g_drag_off_x = mx - hw->x;
            g_drag_off_y = my - (hw->y - TITLEBAR_H);
            return;
        }

        /*  Client area click → forward to app  */
        ipc_msg_t ev;
        ev.type = IPC_MSG_MOUSE_BTN;
        ev.len  = sizeof(mouse_btn_msg_t);
        mouse_btn_msg_t *m = (mouse_btn_msg_t *)ev.data;
        m->x       = mx - hw->x;
        m->y       = my - hw->y;
        m->button  = 1;
        m->pressed = 1;
        ipc_send(hw->owner_pid, &ev);
        return;
    }

    /*  Left button released in client area  */
    if (left_up) {
        if (g_focused < 0) return;
        window_t *fw = &g_windows[g_focused];
        if (!fw->valid) return;
        ipc_msg_t ev;
        ev.type = IPC_MSG_MOUSE_BTN;
        ev.len  = sizeof(mouse_btn_msg_t);
        mouse_btn_msg_t *m = (mouse_btn_msg_t *)ev.data;
        m->x = mx - fw->x; m->y = my - fw->y;
        m->button = 1; m->pressed = 0;
        ipc_send(fw->owner_pid, &ev);
    }
}

/*  keyboard event routing  */

static void route_key(char c)
{
    /* Tab = cycle focus between windows */
    if (c == '\t') {
        if (g_win_count > 1) {
            int next = g_focused + 1;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                int idx = (next + i) % MAX_WINDOWS;
                if (g_windows[idx].valid) {
                    /* Notify old focused window */
                    if (g_focused >= 0 && g_windows[g_focused].valid) {
                        ipc_msg_t blur; blur.type = IPC_MSG_WIN_BLUR; blur.len = 0;
                        ipc_send(g_windows[g_focused].owner_pid, &blur);
                    }
                    g_focused = idx;
                    /* Notify new focused window */
                    ipc_msg_t focus; focus.type = IPC_MSG_WIN_FOCUS; focus.len = 0;
                    ipc_send(g_windows[g_focused].owner_pid, &focus);
                    break;
                }
            }
        }
        return;
    }

    if (g_focused < 0) return;
    window_t *fw = &g_windows[g_focused];
    if (!fw->valid) return;

    ipc_msg_t ev;
    ev.type = IPC_MSG_KEY_DOWN;
    ev.len  = sizeof(key_msg_t);
    key_msg_t *k = (key_msg_t *)ev.data;
    k->scancode = 0;
    k->ascii    = (uint8_t)c;
    ipc_send(fw->owner_pid, &ev);
}

/*  main loop  */

void main(void)
{
    _println("[COMP] Exploidus Compositor starting...");

    /* Write our PID to /tmp/compositor.pid so apps can find us */
    {
        int64_t my_pid = getpid();
        /* Convert PID to decimal string */
        char pidbuf[16]; int pi = 0;
        int64_t tmp = my_pid;
        if (tmp == 0) { pidbuf[pi++] = '0'; }
        else { while (tmp > 0) { pidbuf[pi++] = '0' + (int)(tmp % 10); tmp /= 10; } }
        /* reverse */
        for (int a = 0, b = pi-1; a < b; a++, b--) {
            char t = pidbuf[a]; pidbuf[a] = pidbuf[b]; pidbuf[b] = t;
        }
        pidbuf[pi++] = '\n'; pidbuf[pi] = 0;
        /* Write to /tmp/compositor.pid */
        int pfd = open("/tmp/compositor.pid", O_WRONLY | O_CREAT);
        if (pfd >= 0) { write(pfd, pidbuf, (size_t)pi); close(pfd); }
        _puts("[COMP] PID="); _print_uint((uint64_t)my_pid); _putc('\n');
    }

    /* Get framebuffer info */
    if (fb_info(&g_screen_w, &g_screen_h, &g_fb_active) < 0 || !g_fb_active) {
        _println("[COMP] ERROR: no framebuffer — exiting");
        exit(1);
    }
    _puts("[COMP] Screen "); _print_uint(g_screen_w);
    _puts("x"); _print_uint(g_screen_h); _println("");

    /* Clear window table */
    _memset(g_windows, 0, sizeof(g_windows));

    _println("[COMP] Compositor ready. Entering event loop.");

    /* Claim exclusive keyboard ownership so shell doesn't compete */
    syscall1(SYS_KBD_OWNER_SET, (uint64_t)getpid());

    /* Draw initial desktop so screen isn't blank */
    draw_wallpaper();
    draw_menubar();
    draw_dock();
    fb_flip();

    int32_t prev_mx = 0, prev_my = 0, prev_btn = 0;
    uint64_t frame_tick = 0;
    uint8_t  needs_full_redraw = 1;  /* full scene repaint needed */
    uint8_t  needs_cursor_update = 0; /* just move cursor, no scene rebuild */

    /* Track current mouse pos for cursor draw */
    int32_t cur_mx = 0, cur_my = 0;

    for (;;) {
        /*  1. Drain IPC inbox (non-blocking)  */
        ipc_msg_t msg;
        int got_msg = 0;
        while (ipc_recv_nb(&msg) == 0) {
            got_msg = 1;
            switch (msg.type) {
            case IPC_MSG_WIN_CREATE:
                handle_win_create(&msg);
                needs_full_redraw = 1;
                break;
            case IPC_MSG_WIN_DESTROY:
                handle_win_destroy(&msg);
                needs_full_redraw = 1;
                break;
            case IPC_MSG_DAMAGE:
                handle_damage(&msg);
                needs_full_redraw = 1;
                break;
            case IPC_MSG_WIN_MOVE:
                handle_win_move(&msg);
                needs_full_redraw = 1;
                break;
            case IPC_MSG_PING: {
                ipc_msg_t pong;
                pong.type = IPC_MSG_PONG;
                pong.len  = 0;
                ipc_send(msg.from_pid, &pong);
                break;
            }
            default:
                break;
            }
        }

        /*  2. Poll mouse  */
        int32_t mx, my, btn;
        mouse_pos(&mx, &my, &btn);
        if (mx != prev_mx || my != prev_my) {
            cur_mx = mx; cur_my = my;
            if (g_drag_win >= 0) {
                /* Dragging — route mouse to update window position */
                route_mouse(mx, my, btn, prev_btn);
                needs_full_redraw = 1;
            } else {
                needs_cursor_update = 1;
            }
            prev_mx = mx; prev_my = my;
        }
        if (btn != prev_btn) {
            route_mouse(mx, my, btn, prev_btn);
            prev_btn = btn;
            needs_full_redraw = 1;  /* click may change focus/window state */
        }

        /*  2b. Poll keyboard (non-blocking)  */
        char kc;
        while (kbd_read_nb(&kc) == 1) {
            route_key(kc);
            got_msg = 1;
        }

        /*  3. Redraw  */
        frame_tick++;

        if (needs_full_redraw && (frame_tick % FRAME_TICKS == 0)) {
            /* Full scene composite + cursor + single flip */
            composite_frame();
            draw_rect(cur_mx,     cur_my,     2, 10, COL_TEXT_PRI);
            draw_rect(cur_mx,     cur_my,     10, 2,  COL_TEXT_PRI);
            draw_rect(cur_mx + 1, cur_my + 1, 1,  8,  0x000000);
            draw_rect(cur_mx + 1, cur_my + 1, 8,  1,  0x000000);
            fb_flip();
            needs_full_redraw  = 0;
            needs_cursor_update = 0;

        } else if (needs_cursor_update && (frame_tick % FRAME_TICKS == 0)) {
            /* Cursor-only update — just flip current back-buffer with new cursor */
            /* Repaint small patch around old cursor with background color */
            draw_rect(prev_mx - 1, prev_my - 1, 12, 12, COL_DESKTOP);
            /* Redraw any window pixels the old cursor was covering */
            /* (simplified: just redraw entire scene since we need
             *  to restore what was under the cursor anyway)        */
            composite_frame();
            draw_rect(cur_mx,     cur_my,     2, 10, COL_TEXT_PRI);
            draw_rect(cur_mx,     cur_my,     10, 2,  COL_TEXT_PRI);
            draw_rect(cur_mx + 1, cur_my + 1, 1,  8,  0x000000);
            draw_rect(cur_mx + 1, cur_my + 1, 8,  1,  0x000000);
            fb_flip();
            needs_cursor_update = 0;
        }

        /*  4. Yield if nothing to do  */
        if (!got_msg && !needs_full_redraw && !needs_cursor_update) yield();
    }
}