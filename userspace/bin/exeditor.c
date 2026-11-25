/*
 * exeditor.c — Exploidus Lightweight Cyber Code & Text Editor (Phase 5.5)
 *
 * Graphical text and code editor for Exploidus OS.
 * Features line numbering, syntax highlighting, and ExFS file saving.
 */

#include "../libc/syscall.h"
#include "../compositor/compositor.h"
#include "../compositor/gui_font.h"
#include "../gui/exui/exui.h"

#define WIN_W 560
#define WIN_H 380

#define MAX_LINES 256
#define MAX_LINE_LEN 80

static uint32_t *g_buf = NULL;
static uint32_t  g_comp_pid = 0;
static uint32_t  g_shm_id = 0;

static char      g_filename[64] = "/tmp/note.txt";
static char      g_lines[MAX_LINES][MAX_LINE_LEN];
static int       g_line_count = 1;
static int       g_cursor_row = 0;
static int       g_cursor_col = 0;
static exui_button_t g_btn_save;

static void save_file(void)
{
    int fd = open(g_filename, O_WRONLY | O_CREAT);
    if (fd < 0) return;

    for (int i = 0; i < g_line_count; i++) {
        int len = 0; while (g_lines[i][len]) len++;
        if (len > 0) write(fd, g_lines[i], len);
        write(fd, "\n", 1);
    }
    close(fd);
}

static uint32_t highlight_word(const char *word)
{
    if (strcmp(word, "int") == 0 || strcmp(word, "void") == 0 ||
        strcmp(word, "char") == 0 || strcmp(word, "uint32_t") == 0 ||
        strcmp(word, "uint64_t") == 0 || strcmp(word, "bool") == 0) {
        return EXUI_COL_CYAN;
    }
    if (strcmp(word, "return") == 0 || strcmp(word, "if") == 0 ||
        strcmp(word, "else") == 0 || strcmp(word, "for") == 0 ||
        strcmp(word, "while") == 0) {
        return EXUI_COL_GREEN;
    }
    if (strcmp(word, "static") == 0 || strcmp(word, "const") == 0) {
        return EXUI_COL_AMBER;
    }
    return EXUI_COL_TEXT_PRI;
}

static void render(void)
{
    exui_fill_rect(g_buf, WIN_W, WIN_H, 0, 0, WIN_W, WIN_H, EXUI_COL_BG);

    /* Header toolbar */
    exui_fill_rect(g_buf, WIN_W, WIN_H, 0, 0, WIN_W, 36, EXUI_COL_ELEVATED);
    exui_draw_border(g_buf, WIN_W, WIN_H, 0, 0, WIN_W, 36, EXUI_COL_BORDER);

    exui_button_draw(g_buf, WIN_W, WIN_H, &g_btn_save);

    exui_draw_str_trans(g_buf, WIN_W, WIN_H, 80, 10, "File: ", EXUI_COL_TEXT_SEC);
    exui_draw_str_trans(g_buf, WIN_W, WIN_H, 130, 10, g_filename, EXUI_COL_CYAN);

    /* Editor body */
    int text_y_start = 46;
    int line_h = 16;
    int gutter_w = 40;

    /* Line number gutter */
    exui_fill_rect(g_buf, WIN_W, WIN_H, 0, text_y_start, gutter_w, WIN_H - text_y_start - 24, EXUI_COL_SURFACE);
    exui_fill_rect(g_buf, WIN_W, WIN_H, gutter_w, text_y_start, 1, WIN_H - text_y_start - 24, EXUI_COL_BORDER);

    int max_vis_lines = (WIN_H - text_y_start - 30) / line_h;
    for (int r = 0; r < g_line_count && r < max_vis_lines; r++) {
        int y = text_y_start + r * line_h;

        /* Line number */
        char num[8];
        int val = r + 1, pos = 0;
        while (val) { num[pos++] = '0' + (val % 10); val /= 10; }
        for (int a = 0, b = pos - 1; a < b; a++, b--) { char t = num[a]; num[a] = num[b]; num[b] = t; }
        num[pos] = 0;
        exui_draw_str_trans(g_buf, WIN_W, WIN_H, 12, y, num, EXUI_COL_TEXT_MUTED);

        /* Line text with word-level syntax highlighting */
        const char *line = g_lines[r];
        int x = gutter_w + 10;
        int i = 0;
        while (line[i]) {
            if ((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z') || line[i] == '_') {
                char word[32];
                int w_len = 0;
                while (line[i] && ((line[i] >= 'a' && line[i] <= 'z') ||
                       (line[i] >= 'A' && line[i] <= 'Z') || (line[i] >= '0' && line[i] <= '9') ||
                       line[i] == '_') && w_len < 31) {
                    word[w_len++] = line[i++];
                }
                word[w_len] = 0;
                uint32_t col = highlight_word(word);
                exui_draw_str_trans(g_buf, WIN_W, WIN_H, x, y, word, col);
                x += w_len * GUI_FONT_W;
            } else {
                char ch_s[2] = { line[i++], 0 };
                exui_draw_str_trans(g_buf, WIN_W, WIN_H, x, y, ch_s, EXUI_COL_TEXT_PRI);
                x += GUI_FONT_W;
            }
        }

        /* Cursor */
        if (r == g_cursor_row) {
            int cx = gutter_w + 10 + g_cursor_col * GUI_FONT_W;
            exui_fill_rect(g_buf, WIN_W, WIN_H, cx, y, 2, GUI_FONT_H, EXUI_COL_CYAN);
        }
    }

    /* Bottom status bar */
    int sb_y = WIN_H - 24;
    exui_fill_rect(g_buf, WIN_W, WIN_H, 0, sb_y, WIN_W, 24, EXUI_COL_ELEVATED);
    exui_draw_border(g_buf, WIN_W, WIN_H, 0, sb_y, WIN_W, 24, EXUI_COL_BORDER);
    exui_draw_str_trans(g_buf, WIN_W, WIN_H, 12, sb_y + 4, "MODE: EDIT | UTF-8 | SYNTAX: C/ASM", EXUI_COL_TEXT_SEC);
}

void main(void)
{
    g_comp_pid = compositor_pid();
    if (!g_comp_pid) return;

    g_shm_id = shm_create(WIN_W * WIN_H * 4);
    if (!g_shm_id) return;

    g_buf = (uint32_t *)shm_map(g_shm_id);
    if (!g_buf) return;

    g_btn_save.x = 10; g_btn_save.y = 5; g_btn_save.w = 60; g_btn_save.h = 26;
    g_btn_save.label = "Save"; g_btn_save.accent_col = EXUI_COL_GREEN;

    /* Initial sample text */
    strcpy(g_lines[0], "// Exploidus Core System Script");
    strcpy(g_lines[1], "void start_agent() {");
    strcpy(g_lines[2], "    int status = 1;");
    strcpy(g_lines[3], "    return status;");
    strcpy(g_lines[4], "}");
    g_line_count = 5;
    g_cursor_row = 3;
    g_cursor_col = 18;

    comp_win_create(g_comp_pid, g_shm_id, 140, 100, WIN_W, WIN_H,
                    WIN_FLAG_DECORATED, "ExEditor — Code & Text");

    render();
    comp_damage(g_comp_pid, g_shm_id, 0, 0, WIN_W, WIN_H);

    ipc_msg_t msg;
    for (;;) {
        int64_t r = ipc_recv(&msg);
        if (r < 0) continue;

        if (msg.type == IPC_MSG_MOUSE_BTN) {
            mouse_btn_msg_t *btn = (mouse_btn_msg_t *)msg.data;
            if (btn->button == 1 && btn->pressed) {
                if (exui_button_hit(&g_btn_save, btn->x, btn->y)) {
                    save_file();
                    render();
                    comp_damage(g_comp_pid, g_shm_id, 0, 0, WIN_W, WIN_H);
                }
            }
        } else if (msg.type == IPC_MSG_KEY_DOWN) {
            key_msg_t *k = (key_msg_t *)msg.data;
            char c = (char)k->ascii;

            if (c == '\n' || c == '\r') {
                if (g_line_count < MAX_LINES - 1) {
                    for (int i = g_line_count; i > g_cursor_row + 1; i--) {
                        strcpy(g_lines[i], g_lines[i - 1]);
                    }
                    g_lines[g_cursor_row + 1][0] = 0;
                    g_line_count++;
                    g_cursor_row++;
                    g_cursor_col = 0;
                }
            } else if (c == '\b') {
                if (g_cursor_col > 0) {
                    g_cursor_col--;
                    g_lines[g_cursor_row][g_cursor_col] = 0;
                }
            } else if (c >= 32 && c <= 126 && g_cursor_col < MAX_LINE_LEN - 1) {
                g_lines[g_cursor_row][g_cursor_col++] = c;
                g_lines[g_cursor_row][g_cursor_col] = 0;
            }
            render();
            comp_damage(g_comp_pid, g_shm_id, 0, 0, WIN_W, WIN_H);
        } else if (msg.type == IPC_MSG_WIN_DESTROY) {
            break;
        }
    }
}
