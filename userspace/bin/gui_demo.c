/*
 * gui_demo.c — First GUI app using the compositor protocol
 *
 * Opens a window via the compositor, draws into its own SHM pixel
 * buffer, then loops processing events from the compositor.
 *
 * Compile with SHELL_CFLAGS, link with auditd.ld (or compositor.ld).
 * Spawn from init or exploish: spawn("/bin/gui_demo")
 */

/* __EXPLOIDUS_USERSPACE__ defined by -D flag in SHELL_CFLAGS */
#include "../libc/syscall.h"
#include "../compositor/compositor.h"

/* Window dimensions */
#define WIN_W  400
#define WIN_H  280

/* Color helpers */
#define RGB(r,g,b) (0xFF000000u | ((uint32_t)(r)<<16) | ((uint32_t)(g)<<8) | (uint32_t)(b))
#define COL_BG      0xFF161B22u
#define COL_HEADER  0xFF1F2937u
#define COL_BORDER  0xFF30363Du
#define COL_TEXT    0xFFE6EDF3u
#define COL_ACCENT  0xFFA78BFAu
#define COL_GREEN   0xFF10B981u
#define COL_BTN     0xFF2D333Bu

static uint32_t *g_buf;   /* points directly into SHM — no separate static buffer */

/*  drawing into our own pixel buffer  */

static void buf_fill(int x, int y, int w, int h, uint32_t col)
{
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= WIN_H) continue;
        for (int col2 = x; col2 < x + w; col2++) {
            if (col2 < 0 || col2 >= WIN_W) continue;
            g_buf[row * WIN_W + col2] = col;
        }
    }
}

static void buf_pixel(int x, int y, uint32_t col)
{
    if (x < 0 || x >= WIN_W || y < 0 || y >= WIN_H) return;
    g_buf[y * WIN_W + x] = col;
}

static void buf_hline(int x, int y, int len, uint32_t col)
{
    buf_fill(x, y, len, 1, col);
}

static void buf_vline(int x, int y, int len, uint32_t col)
{
    buf_fill(x, y, 1, len, col);
}

/* Simple 8-pixel-wide bitmap font blit */
static void buf_char(int x, int y, unsigned char c, uint32_t fg)
{
    /* Minimal 5x7 glyphs for printable ASCII */
    /* We use the kernel's font indirectly — since we can't call
     * font_draw_char from userspace, we write directly to the SHM
     * buffer using a very compact 1bpp 5x7 table.              */
    static const uint8_t glyphs[96][5] = {
        {0x00,0x00,0x00,0x00,0x00}, /* space */
        {0x00,0x00,0x5F,0x00,0x00}, /* ! */
        {0x00,0x07,0x00,0x07,0x00}, /* " */
        {0x14,0x7F,0x14,0x7F,0x14}, /* # */
        {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
        {0x23,0x13,0x08,0x64,0x62}, /* % */
        {0x36,0x49,0x55,0x22,0x50}, /* & */
        {0x00,0x05,0x03,0x00,0x00}, /* ' */
        {0x00,0x1C,0x22,0x41,0x00}, /* ( */
        {0x00,0x41,0x22,0x1C,0x00}, /* ) */
        {0x14,0x08,0x3E,0x08,0x14}, /* * */
        {0x08,0x08,0x3E,0x08,0x08}, /* + */
        {0x00,0x50,0x30,0x00,0x00}, /* , */
        {0x08,0x08,0x08,0x08,0x08}, /* - */
        {0x00,0x60,0x60,0x00,0x00}, /* . */
        {0x20,0x10,0x08,0x04,0x02}, /* / */
        {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
        {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
        {0x42,0x61,0x51,0x49,0x46}, /* 2 */
        {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
        {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
        {0x27,0x45,0x45,0x45,0x39}, /* 5 */
        {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
        {0x01,0x71,0x09,0x05,0x03}, /* 7 */
        {0x36,0x49,0x49,0x49,0x36}, /* 8 */
        {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
        /* A-Z and a-z simplified */
        {0x00,0x00,0x14,0x00,0x00}, /* : */
        {0x00,0x40,0x34,0x00,0x00}, /* ; */
        {0x08,0x14,0x22,0x41,0x00}, /* < */
        {0x14,0x14,0x14,0x14,0x14}, /* = */
        {0x00,0x41,0x22,0x14,0x08}, /* > */
        {0x02,0x01,0x51,0x09,0x06}, /* ? */
        {0x32,0x49,0x79,0x41,0x3E}, /* @ */
        {0x7E,0x11,0x11,0x11,0x7E}, /* A */
        {0x7F,0x49,0x49,0x49,0x36}, /* B */
        {0x3E,0x41,0x41,0x41,0x22}, /* C */
        {0x7F,0x41,0x41,0x22,0x1C}, /* D */
        {0x7F,0x49,0x49,0x49,0x41}, /* E */
        {0x7F,0x09,0x09,0x09,0x01}, /* F */
        {0x3E,0x41,0x49,0x49,0x7A}, /* G */
        {0x7F,0x08,0x08,0x08,0x7F}, /* H */
        {0x00,0x41,0x7F,0x41,0x00}, /* I */
        {0x20,0x40,0x41,0x3F,0x01}, /* J */
        {0x7F,0x08,0x14,0x22,0x41}, /* K */
        {0x7F,0x40,0x40,0x40,0x40}, /* L */
        {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
        {0x7F,0x04,0x08,0x10,0x7F}, /* N */
        {0x3E,0x41,0x41,0x41,0x3E}, /* O */
        {0x7F,0x09,0x09,0x09,0x06}, /* P */
        {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
        {0x7F,0x09,0x19,0x29,0x46}, /* R */
        {0x46,0x49,0x49,0x49,0x31}, /* S */
        {0x01,0x01,0x7F,0x01,0x01}, /* T */
        {0x3F,0x40,0x40,0x40,0x3F}, /* U */
        {0x1F,0x20,0x40,0x20,0x1F}, /* V */
        {0x3F,0x40,0x38,0x40,0x3F}, /* W */
        {0x63,0x14,0x08,0x14,0x63}, /* X */
        {0x07,0x08,0x70,0x08,0x07}, /* Y */
        {0x61,0x51,0x49,0x45,0x43}, /* Z */
        {0x00,0x7F,0x41,0x41,0x00}, /* [ */
        {0x02,0x04,0x08,0x10,0x20}, /* \ */
        {0x00,0x41,0x41,0x7F,0x00}, /* ] */
        {0x04,0x02,0x01,0x02,0x04}, /* ^ */
        {0x40,0x40,0x40,0x40,0x40}, /* _ */
        {0x00,0x01,0x02,0x04,0x00}, /* ` */
        {0x20,0x54,0x54,0x54,0x78}, /* a */
        {0x7F,0x48,0x44,0x44,0x38}, /* b */
        {0x38,0x44,0x44,0x44,0x20}, /* c */
        {0x38,0x44,0x44,0x48,0x7F}, /* d */
        {0x38,0x54,0x54,0x54,0x18}, /* e */
        {0x08,0x7E,0x09,0x01,0x02}, /* f */
        {0x0C,0x52,0x52,0x52,0x3E}, /* g */
        {0x7F,0x08,0x04,0x04,0x78}, /* h */
        {0x00,0x44,0x7D,0x40,0x00}, /* i */
        {0x20,0x40,0x44,0x3D,0x00}, /* j */
        {0x7F,0x10,0x28,0x44,0x00}, /* k */
        {0x00,0x41,0x7F,0x40,0x00}, /* l */
        {0x7C,0x04,0x18,0x04,0x78}, /* m */
        {0x7C,0x08,0x04,0x04,0x78}, /* n */
        {0x38,0x44,0x44,0x44,0x38}, /* o */
        {0x7C,0x14,0x14,0x14,0x08}, /* p */
        {0x08,0x14,0x14,0x18,0x7C}, /* q */
        {0x7C,0x08,0x04,0x04,0x08}, /* r */
        {0x48,0x54,0x54,0x54,0x20}, /* s */
        {0x04,0x3F,0x44,0x40,0x20}, /* t */
        {0x3C,0x40,0x40,0x40,0x7C}, /* u */
        {0x1C,0x20,0x40,0x20,0x1C}, /* v */
        {0x3C,0x40,0x30,0x40,0x3C}, /* w */
        {0x44,0x28,0x10,0x28,0x44}, /* x */
        {0x0C,0x50,0x50,0x50,0x3C}, /* y */
        {0x44,0x64,0x54,0x4C,0x44}, /* z */
        {0x00,0x08,0x36,0x41,0x00}, /* { */
        {0x00,0x00,0x7F,0x00,0x00}, /* | */
        {0x00,0x41,0x36,0x08,0x00}, /* } */
        {0x10,0x08,0x08,0x10,0x08}, /* ~ */
        {0x00,0x00,0x00,0x00,0x00}, /* DEL */
    };

    if (c < 32 || c > 127) return;
    const uint8_t *g = glyphs[(int)(c - 32)];
    for (int col = 0; col < 5; col++) {
        uint8_t col_data = g[col];
        for (int row = 0; row < 7; row++) {
            if (col_data & (1 << row))
                buf_pixel(x + col, y + row, fg);
        }
    }
}

static void buf_str(int x, int y, const char *s, uint32_t fg)
{
    while (*s) {
        buf_char(x, y, (unsigned char)(*s++), fg);
        x += 6;
    }
}

/*  render the window contents  */

static int g_click_count = 0;

static void render(void)
{
    /* Background */
    buf_fill(0, 0, WIN_W, WIN_H, COL_BG);

    /* Header strip */
    buf_fill(0, 0, WIN_W, 36, COL_HEADER);
    buf_hline(0, 36, WIN_W, COL_BORDER);
    buf_str(14, 12, "Exploidus GUI - Phase 2", COL_ACCENT);

    /* Body content */
    buf_str(20, 56, "Hello from a compositor-managed window!", COL_TEXT);
    buf_str(20, 74, "This pixel buffer lives in shared memory.", 0xFFC0C0C0u);
    buf_str(20, 92, "The compositor blends it onto the screen.", 0xFFC0C0C0u);

    /* Divider */
    buf_hline(20, 115, WIN_W - 40, COL_BORDER);

    /* Status box */
    buf_fill(20, 130, WIN_W - 40, 50, 0xFF0D1117u);
    buf_hline(20, 130, WIN_W - 40, COL_BORDER);
    buf_hline(20, 179, WIN_W - 40, COL_BORDER);
    buf_vline(20,  130, 50, COL_BORDER);
    buf_vline(WIN_W - 21, 130, 50, COL_BORDER);

    buf_str(30, 140, "Window: 400x280  Format: ARGB32", 0xFFC0C0C0u);
    buf_str(30, 156, "Protocol: SHM + IPC  Phase: 2", 0xFFC0C0C0u);

    /* Click counter */
    buf_str(20, 200, "Clicks: ", COL_TEXT);
    char cnt[8];
    int n = g_click_count; int i = 0;
    if (!n) { cnt[i++] = '0'; }
    else { int tmp=n; while(tmp){cnt[i++]='0'+tmp%10; tmp/=10;} }
    /* reverse */
    for (int a=0,b=i-1;a<b;a++,b--){char t=cnt[a];cnt[a]=cnt[b];cnt[b]=t;}
    cnt[i] = 0;
    buf_str(20 + 8*8, 200, cnt, COL_ACCENT);

    /* Button */
    buf_fill(WIN_W/2 - 60, WIN_H - 44, 120, 28, COL_BTN);
    buf_hline(WIN_W/2-60, WIN_H-44, 120, COL_BORDER);
    buf_hline(WIN_W/2-60, WIN_H-17, 120, COL_BORDER);
    buf_vline(WIN_W/2-60, WIN_H-44, 28, COL_BORDER);
    buf_vline(WIN_W/2+59, WIN_H-44, 28, COL_BORDER);
    buf_str(WIN_W/2-36, WIN_H-34, "Click me!", COL_ACCENT);

    /* Bottom border */
    buf_hline(0, WIN_H-1, WIN_W, COL_BORDER);
}

/*  main  */

void main(void)
{
    /* 0. Find compositor PID */
    uint32_t comp_pid = 0;
    /* Retry a few times — compositor may still be initializing */
    for (int tries = 0; tries < 20 && !comp_pid; tries++) {
        comp_pid = compositor_pid();
        if (!comp_pid) sleep_ticks(5);
    }
    if (!comp_pid) {
        write(1, "gui_demo: compositor not found\n", 31);
        exit(1);
    }
    /* write comp_pid to stdout for debug */
    {
        char msg[] = "gui_demo: compositor PID=";
        write(1, msg, sizeof(msg)-1);
        char nb[8]; int ni=0; uint32_t tmp=comp_pid;
        if(!tmp){nb[ni++]='0';}
        else{while(tmp){nb[ni++]='0'+tmp%10;tmp/=10;}}
        for(int a=0,b=ni-1;a<b;a++,b--){char t=nb[a];nb[a]=nb[b];nb[b]=t;}
        nb[ni++]='\n'; write(1,nb,(size_t)ni);
    }
    /* 1. Create SHM pixel buffer */
    uint32_t shm_id = shm_create((uint64_t)(WIN_W * WIN_H * 4));
    if (!shm_id) {
        write(1, "gui_demo: shm_create failed\n", 28);
        exit(1);
    }

    /* 2. Map SHM into our address space — use it directly as pixel buffer */
    uint32_t *mapped = (uint32_t *)shm_map(shm_id);
    if (!mapped) {
        write(1, "gui_demo: shm_map failed\n", 25);
        exit(1);
    }
    /* Draw directly into SHM — no separate g_buf copy needed */
    g_buf = mapped;

    /* 3. Initial render directly into SHM */
    render();

    /* 4. Register window with compositor */
    int32_t wx = 80, wy = 80;
    if (comp_win_create(comp_pid, shm_id, wx, wy, WIN_W, WIN_H,
                        WIN_FLAG_DECORATED,
                        "GUI Demo") < 0) {
        write(1, "gui_demo: comp_win_create failed\n", 33);
        exit(1);
    }

    /* Signal compositor to repaint */
    comp_damage(comp_pid, shm_id, 0, 0, WIN_W, WIN_H);

    /* 5. Event loop */
    ipc_msg_t msg;
    for (;;) {
        ipc_recv(&msg);   /* blocking — sleep until event */

        switch (msg.type) {
        case IPC_MSG_MOUSE_BTN: {
            mouse_btn_msg_t *m = (mouse_btn_msg_t *)msg.data;
            if (m->pressed && m->button == 1) {
                g_click_count++;
                render();
                comp_damage(comp_pid, shm_id, 0, 0, WIN_W, WIN_H);
            }
            break;
        }
        case IPC_MSG_WIN_BLUR:
        case IPC_MSG_WIN_FOCUS:
            /* Re-render on focus change */
            render();
            comp_damage(comp_pid, shm_id, 0, 0, WIN_W, WIN_H);
            break;
        default:
            break;
        }
    }

    /* Cleanup (unreachable in demo) */
    comp_win_destroy(comp_pid, shm_id);
    shm_unmap(mapped, (uint64_t)(WIN_W * WIN_H * 4));
    shm_destroy(shm_id);
}