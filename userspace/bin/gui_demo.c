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
#include "../compositor/gui_font.h"

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

static void buf_hline(int x, int y, int len, uint32_t col)
{
    buf_fill(x, y, len, 1, col);
}

static void buf_vline(int x, int y, int len, uint32_t col)
{
    buf_fill(x, y, 1, len, col);
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
    gui_font_str(g_buf, WIN_W, WIN_H, 14, 12, "Exploidus GUI", COL_ACCENT, 0);

    /* Body content */
    gui_font_str(g_buf, WIN_W, WIN_H, 20, 56, "Hello from a compositor-managed window!", COL_TEXT, 0);
    gui_font_str(g_buf, WIN_W, WIN_H, 20, 74, "This pixel buffer lives in shared memory.", 0xFFC0C0C0u, 0);
    gui_font_str(g_buf, WIN_W, WIN_H, 20, 92, "The compositor blends it onto the screen.", 0xFFC0C0C0u, 0);

    /* Divider */
    buf_hline(20, 115, WIN_W - 40, COL_BORDER);

    /* Status box */
    buf_fill(20, 130, WIN_W - 40, 50, 0xFF0D1117u);
    buf_hline(20, 130, WIN_W - 40, COL_BORDER);
    buf_hline(20, 179, WIN_W - 40, COL_BORDER);
    buf_vline(20,  130, 50, COL_BORDER);
    buf_vline(WIN_W - 21, 130, 50, COL_BORDER);

    gui_font_str(g_buf, WIN_W, WIN_H, 30, 140, "Window: 400x280  Format: ARGB32", 0xFFC0C0C0u, 0);
    gui_font_str(g_buf, WIN_W, WIN_H, 30, 156, "Protocol: SHM + IPC  Phase: 2", 0xFFC0C0C0u, 0);

    /* Click counter */
    gui_font_str(g_buf, WIN_W, WIN_H, 20, 200, "Clicks: ", COL_TEXT, 0);
    char cnt[8];
    int n = g_click_count; int i = 0;
    if (!n) { cnt[i++] = '0'; }
    else { int tmp=n; while(tmp){cnt[i++]='0'+tmp%10; tmp/=10;} }
    /* reverse */
    for (int a=0,b=i-1;a<b;a++,b--){char t=cnt[a];cnt[a]=cnt[b];cnt[b]=t;}
    cnt[i] = 0;
    gui_font_str(g_buf, WIN_W, WIN_H, 20 + 8*8, 200, cnt, COL_ACCENT, 0);

    /* Button */
    buf_fill(WIN_W/2 - 60, WIN_H - 44, 120, 28, COL_BTN);
    buf_hline(WIN_W/2-60, WIN_H-44, 120, COL_BORDER);
    buf_hline(WIN_W/2-60, WIN_H-17, 120, COL_BORDER);
    buf_vline(WIN_W/2-60, WIN_H-44, 28, COL_BORDER);
    buf_vline(WIN_W/2+59, WIN_H-44, 28, COL_BORDER);
    gui_font_str(g_buf, WIN_W, WIN_H, WIN_W/2-36, WIN_H-34, "Click me!", COL_ACCENT, 0);

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