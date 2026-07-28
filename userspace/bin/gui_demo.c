/*
 * gui_demo.c -- Exploidus GUI Widget Toolkit showcase
 *
 * Opens a window via the compositor and builds its UI entirely out
 * of gui_widgets.h (button, label, checkbox, textbox) instead of
 * hand-rolled rect/text calls -- this is both a demo app and the
 * reference example for how to use the widget toolkit.
 *
 * Compile with SHELL_CFLAGS, link with fixed.ld.
 * Spawn from init or exploish: spawn("/bin/gui_demo")
 */

/* __EXPLOIDUS_USERSPACE__ defined by -D flag in SHELL_CFLAGS */
#include "../libc/syscall.h"
#include "../compositor/compositor.h"
#include "../compositor/gui_font.h"
#include "../compositor/gui_widgets.h"

/* Window dimensions */
#define WIN_W  400
#define WIN_H  280

static uint32_t *g_buf;   /* points directly into SHM -- no separate copy needed */

static gui_button_t   g_btn      = { .x = WIN_W/2 - 60, .y = WIN_H - 44, .w = 120, .h = 28,
                                      .label = "Click me!" };
static gui_checkbox_t g_check    = { .x = 20, .y = 178, .label = "Enable widget demo" };
static gui_textbox_t  g_textbox  = { .x = 20, .y = 130, .w = WIN_W - 40, .h = 28 };
static int             g_textbox_focused = 0;
static int             g_click_count = 0;

/*  render the window contents using gui_widgets.h  */

static void render(void)
{
    gui_fill_rect(g_buf, WIN_W, WIN_H, 0, 0, WIN_W, WIN_H, GUI_COL_SURFACE);

    /* Header strip */
    gui_fill_rect(g_buf, WIN_W, WIN_H, 0, 0, WIN_W, 36, GUI_COL_ELEVATED);
    gui_fill_rect(g_buf, WIN_W, WIN_H, 0, 36, WIN_W, 1, GUI_COL_BORDER);
    gui_label_t title = { .x = 14, .y = 12, .text = "Exploidus Widget Toolkit",
                           .color = GUI_COL_ACCENT_PUR };
    gui_label_draw(g_buf, WIN_W, WIN_H, &title);

    gui_label_t l1 = { .x = 20, .y = 56, .text = "Every widget below comes from gui_widgets.h,",
                        .color = GUI_COL_TEXT_SEC };
    gui_label_draw(g_buf, WIN_W, WIN_H, &l1);
    gui_label_t l2 = { .x = 20, .y = 74, .text = "the shared toolkit any Exploidus GUI app can use.",
                        .color = GUI_COL_TEXT_SEC };
    gui_label_draw(g_buf, WIN_W, WIN_H, &l2);

    gui_label_t tb_lbl = { .x = 20, .y = 114, .text = "Textbox (click to focus, type):",
                            .color = GUI_COL_TEXT_PRI };
    gui_label_draw(g_buf, WIN_W, WIN_H, &tb_lbl);
    gui_textbox_draw(g_buf, WIN_W, WIN_H, &g_textbox, g_textbox_focused);

    gui_checkbox_draw(g_buf, WIN_W, WIN_H, &g_check);

    gui_label_t clicks_lbl = { .x = 20, .y = 210, .text = "Clicks:", .color = GUI_COL_TEXT_PRI };
    gui_label_draw(g_buf, WIN_W, WIN_H, &clicks_lbl);
    char cnt[8];
    int n = g_click_count, i = 0;
    if (!n) { cnt[i++] = '0'; }
    else { int tmp = n; while (tmp) { cnt[i++] = '0' + tmp % 10; tmp /= 10; } }
    for (int a = 0, b = i - 1; a < b; a++, b--) { char t = cnt[a]; cnt[a] = cnt[b]; cnt[b] = t; }
    cnt[i] = 0;
    gui_label_t clicks_val = { .x = 20 + 8 * GUI_FONT_W, .y = 210, .text = cnt,
                                .color = GUI_COL_ACCENT_PUR };
    gui_label_draw(g_buf, WIN_W, WIN_H, &clicks_val);

    gui_button_draw(g_buf, WIN_W, WIN_H, &g_btn);

    gui_fill_rect(g_buf, WIN_W, WIN_H, 0, WIN_H - 1, WIN_W, 1, GUI_COL_BORDER);
}

/*  main  */

void main(void)
{
    /* 0. Find compositor PID */
    uint32_t comp_pid = 0;
    /* Retry a few times -- compositor may still be initializing */
    for (int tries = 0; tries < 20 && !comp_pid; tries++) {
        comp_pid = compositor_pid();
        if (!comp_pid) sleep_ticks(5);
    }
    if (!comp_pid) {
        write(1, "gui_demo: compositor not found\n", 31);
        exit(1);
    }

    /* 1. Create SHM pixel buffer */
    uint32_t shm_id = shm_create((uint64_t)(WIN_W * WIN_H * 4));
    if (!shm_id) {
        write(1, "gui_demo: shm_create failed\n", 28);
        exit(1);
    }

    /* 2. Map SHM into our address space -- use it directly as pixel buffer */
    uint32_t *mapped = (uint32_t *)shm_map(shm_id);
    if (!mapped) {
        write(1, "gui_demo: shm_map failed\n", 25);
        exit(1);
    }
    g_buf = mapped;

    /* 3. Initial render directly into SHM */
    render();

    /* 4. Register window with compositor */
    int32_t wx = 80, wy = 80;
    if (comp_win_create(comp_pid, shm_id, wx, wy, WIN_W, WIN_H,
                        WIN_FLAG_DECORATED,
                        "Widget Demo") < 0) {
        write(1, "gui_demo: comp_win_create failed\n", 33);
        exit(1);
    }

    comp_damage(comp_pid, shm_id, 0, 0, WIN_W, WIN_H);

    /* 5. Event loop */
    ipc_msg_t msg;
    for (;;) {
        ipc_recv(&msg);   /* blocking -- sleep until event */

        switch (msg.type) {
        case IPC_MSG_MOUSE_BTN: {
            mouse_btn_msg_t *m = (mouse_btn_msg_t *)msg.data;
            if (m->pressed && m->button == 1) {
                if (gui_button_hit(&g_btn, m->x, m->y)) {
                    g_btn.state = GUI_BTN_PRESSED;
                    g_click_count++;
                    g_textbox_focused = 0;
                } else if (gui_checkbox_hit(&g_check, m->x, m->y)) {
                    g_check.checked = !g_check.checked;
                    g_textbox_focused = 0;
                } else if (gui_textbox_hit(&g_textbox, m->x, m->y)) {
                    g_textbox_focused = 1;
                    g_textbox.cursor_visible = 1;
                } else {
                    g_textbox_focused = 0;
                }
                render();
                comp_damage(comp_pid, shm_id, 0, 0, WIN_W, WIN_H);
            } else if (!m->pressed && g_btn.state == GUI_BTN_PRESSED) {
                g_btn.state = GUI_BTN_NORMAL;
                render();
                comp_damage(comp_pid, shm_id, 0, 0, WIN_W, WIN_H);
            }
            break;
        }
        case IPC_MSG_KEY_DOWN: {
            key_msg_t *k = (key_msg_t *)msg.data;
            if (g_textbox_focused) {
                if (gui_textbox_input(&g_textbox, (char)k->ascii)) {
                    render();
                    comp_damage(comp_pid, shm_id, 0, 0, WIN_W, WIN_H);
                }
            }
            break;
        }
        case IPC_MSG_WIN_BLUR:
        case IPC_MSG_WIN_FOCUS:
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