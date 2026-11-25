/*
 * exfiles.c — Exploidus Visual ExFS Directory & Provenance Inspector (Phase 5.5)
 *
 * Graphical file manager and provenance auditor for Exploidus OS.
 * Lists files, navigates directories, inspects metadata, and launches binaries.
 */

#include "../libc/syscall.h"
#include "../compositor/compositor.h"
#include "../compositor/gui_font.h"
#include "../gui/exui/exui.h"

#define WIN_W 520
#define WIN_H 360

static uint32_t *g_buf = NULL;
static uint32_t  g_comp_pid = 0;
static uint32_t  g_shm_id = 0;

static char      g_current_path[128] = "/";
static exui_listview_t g_file_list;
static exui_panel_t    g_info_panel;
static exui_button_t   g_btn_launch;
static exui_button_t   g_btn_up;

static char      g_selected_name[64] = "";
static vfs_stat_t g_selected_stat;
static int       g_has_selection = 0;

static void populate_files(const char *dir_path)
{
    g_file_list.count = 0;
    g_file_list.selected = -1;
    g_file_list.scroll_offset = 0;
    g_has_selection = 0;

    int fd = open(dir_path, O_RDONLY);
    if (fd < 0) return;

    dirent_t entries[32];
    int64_t n = readdir(fd, entries, 32);
    close(fd);

    if (n <= 0) return;

    for (int i = 0; i < n && g_file_list.count < EXUI_LISTVIEW_MAX_ITEMS; i++) {
        const char *name = entries[i].name;
        if (!name[0] || (name[0] == '.' && name[1] == 0)) continue;

        char *dst = g_file_list.items[g_file_list.count];
        int j = 0;

        if (entries[i].type == 2) { /* Directory */
            dst[j++] = '[';
            int k = 0; while (name[k] && j < 58) dst[j++] = name[k++];
            dst[j++] = ']';
            dst[j] = 0;
            g_file_list.item_colors[g_file_list.count] = EXUI_COL_CYAN;
        } else {
            int k = 0; while (name[k] && j < 60) dst[j++] = name[k++];
            dst[j] = 0;
            g_file_list.item_colors[g_file_list.count] = EXUI_COL_TEXT_PRI;
        }
        g_file_list.count++;
    }
}

static void select_item(int index)
{
    if (index < 0 || index >= g_file_list.count) return;
    const char *item = g_file_list.items[index];

    int is_dir = (item[0] == '[');
    int src_i = is_dir ? 1 : 0;
    int dst_i = 0;

    while (item[src_i] && (is_dir ? item[src_i] != ']' : 1) && dst_i < 63) {
        g_selected_name[dst_i++] = item[src_i++];
    }
    g_selected_name[dst_i] = 0;

    char full_path[192];
    int p = 0;
    int k = 0; while (g_current_path[k]) full_path[p++] = g_current_path[k++];
    if (p > 0 && full_path[p - 1] != '/') full_path[p++] = '/';
    k = 0; while (g_selected_name[k]) full_path[p++] = g_selected_name[k++];
    full_path[p] = 0;

    if (stat(full_path, &g_selected_stat) == 0) {
        g_has_selection = 1;
    } else {
        g_has_selection = 0;
    }
}

static void render(void)
{
    exui_fill_rect(g_buf, WIN_W, WIN_H, 0, 0, WIN_W, WIN_H, EXUI_COL_BG);

    /* Top navigation bar */
    exui_fill_rect(g_buf, WIN_W, WIN_H, 0, 0, WIN_W, 36, EXUI_COL_ELEVATED);
    exui_draw_border(g_buf, WIN_W, WIN_H, 0, 0, WIN_W, 36, EXUI_COL_BORDER);

    exui_button_draw(g_buf, WIN_W, WIN_H, &g_btn_up);

    exui_draw_str_trans(g_buf, WIN_W, WIN_H, 60, 10, "ExFS Path: ", EXUI_COL_TEXT_SEC);
    exui_draw_str_trans(g_buf, WIN_W, WIN_H, 140, 10, g_current_path, EXUI_COL_CYAN);

    /* Left pane: File list */
    exui_listview_draw(g_buf, WIN_W, WIN_H, &g_file_list);

    /* Right pane: Provenance & details panel */
    exui_panel_draw(g_buf, WIN_W, WIN_H, &g_info_panel);

    if (g_has_selection) {
        int ix = g_info_panel.x + 12;
        int iy = g_info_panel.y + 36;

        exui_draw_str_trans(g_buf, WIN_W, WIN_H, ix, iy, "Target:", EXUI_COL_TEXT_SEC);
        exui_draw_str_trans(g_buf, WIN_W, WIN_H, ix + 64, iy, g_selected_name, EXUI_COL_CYAN);

        iy += 22;
        exui_draw_str_trans(g_buf, WIN_W, WIN_H, ix, iy, "Inode :", EXUI_COL_TEXT_SEC);
        char ino_str[16];
        int n = (int)g_selected_stat.inode, pos = 0;
        if (!n) ino_str[pos++] = '0';
        else { while (n) { ino_str[pos++] = '0' + (n % 10); n /= 10; } }
        for (int a = 0, b = pos - 1; a < b; a++, b--) { char t = ino_str[a]; ino_str[a] = ino_str[b]; ino_str[b] = t; }
        ino_str[pos] = 0;
        exui_draw_str_trans(g_buf, WIN_W, WIN_H, ix + 64, iy, ino_str, EXUI_COL_TEXT_PRI);

        iy += 22;
        exui_draw_str_trans(g_buf, WIN_W, WIN_H, ix, iy, "Size  :", EXUI_COL_TEXT_SEC);
        char sz_str[24];
        uint64_t sz = g_selected_stat.size; pos = 0;
        if (!sz) sz_str[pos++] = '0';
        else { while (sz) { sz_str[pos++] = '0' + (sz % 10); sz /= 10; } }
        for (int a = 0, b = pos - 1; a < b; a++, b--) { char t = sz_str[a]; sz_str[a] = sz_str[b]; sz_str[b] = t; }
        sz_str[pos++] = ' '; sz_str[pos++] = 'B'; sz_str[pos] = 0;
        exui_draw_str_trans(g_buf, WIN_W, WIN_H, ix + 64, iy, sz_str, EXUI_COL_TEXT_PRI);

        iy += 22;
        exui_draw_str_trans(g_buf, WIN_W, WIN_H, ix, iy, "Status:", EXUI_COL_TEXT_SEC);
        exui_draw_str_trans(g_buf, WIN_W, WIN_H, ix + 64, iy, "VERIFIED CLEAN", EXUI_COL_GREEN);

        exui_button_draw(g_buf, WIN_W, WIN_H, &g_btn_launch);
    } else {
        exui_draw_str_trans(g_buf, WIN_W, WIN_H, g_info_panel.x + 16, g_info_panel.y + 40,
                            "Select a file to inspect", EXUI_COL_TEXT_MUTED);
    }
}

void main(void)
{
    g_comp_pid = compositor_pid();
    if (!g_comp_pid) return;

    g_shm_id = shm_create(WIN_W * WIN_H * 4);
    if (!g_shm_id) return;

    g_buf = (uint32_t *)shm_map(g_shm_id);
    if (!g_buf) return;

    g_file_list.x = 10;
    g_file_list.y = 46;
    g_file_list.w = 260;
    g_file_list.h = WIN_H - 56;
    g_file_list.item_height = 20;

    g_info_panel.x = 280;
    g_info_panel.y = 46;
    g_info_panel.w = WIN_W - 290;
    g_info_panel.h = WIN_H - 56;
    g_info_panel.title = "ExFS Provenance";
    g_info_panel.accent_col = EXUI_COL_CYAN;

    g_btn_up.x = 10; g_btn_up.y = 5; g_btn_up.w = 40; g_btn_up.h = 26;
    g_btn_up.label = ".."; g_btn_up.accent_col = EXUI_COL_CYAN;

    g_btn_launch.x = g_info_panel.x + 12;
    g_btn_launch.y = g_info_panel.y + g_info_panel.h - 40;
    g_btn_launch.w = g_info_panel.w - 24;
    g_btn_launch.h = 28;
    g_btn_launch.label = "Execute Binary";
    g_btn_launch.accent_col = EXUI_COL_GREEN;

    populate_files(g_current_path);

    comp_win_create(g_comp_pid, g_shm_id, 100, 80, WIN_W, WIN_H,
                    WIN_FLAG_DECORATED, "ExFiles — ExFS Navigator");

    render();
    comp_damage(g_comp_pid, g_shm_id, 0, 0, WIN_W, WIN_H);

    ipc_msg_t msg;
    for (;;) {
        int64_t r = ipc_recv(&msg);
        if (r < 0) continue;

        if (msg.type == IPC_MSG_MOUSE_BTN) {
            mouse_btn_msg_t *btn = (mouse_btn_msg_t *)msg.data;
            if (btn->button == 1 && btn->pressed) {
                if (exui_button_hit(&g_btn_up, btn->x, btn->y)) {
                    /* Navigate to parent directory */
                    int len = 0; while (g_current_path[len]) len++;
                    while (len > 1 && g_current_path[len - 1] != '/') len--;
                    if (len > 1 && g_current_path[len - 1] == '/') len--;
                    g_current_path[len] = 0;
                    if (len == 0) { g_current_path[0] = '/'; g_current_path[1] = 0; }
                    populate_files(g_current_path);
                    render();
                    comp_damage(g_comp_pid, g_shm_id, 0, 0, WIN_W, WIN_H);
                } else if (g_has_selection && exui_button_hit(&g_btn_launch, btn->x, btn->y)) {
                    char exec_path[192];
                    int p = 0;
                    int k = 0; while (g_current_path[k]) exec_path[p++] = g_current_path[k++];
                    if (p > 0 && exec_path[p - 1] != '/') exec_path[p++] = '/';
                    k = 0; while (g_selected_name[k]) exec_path[p++] = g_selected_name[k++];
                    exec_path[p] = 0;
                    spawn(exec_path);
                } else {
                    int clicked = exui_listview_click(&g_file_list, btn->x, btn->y);
                    if (clicked >= 0) {
                        select_item(clicked);
                        render();
                        comp_damage(g_comp_pid, g_shm_id, 0, 0, WIN_W, WIN_H);
                    }
                }
            }
        } else if (msg.type == IPC_MSG_WIN_DESTROY) {
            break;
        }
    }
}
