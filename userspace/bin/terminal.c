/*
 * terminal.c — Exploidus GUI Terminal (Phase 3)
 *
 * A compositor-managed window containing a character grid terminal.
 * Receives IPC_MSG_KEY_DOWN events from the compositor, builds a
 * command line, executes a small built-in command set (no fork/exec
 * needed for basics), and renders monospace text into its SHM buffer.
 *
 * This is intentionally a separate, minimal shell — not a wrapper
 * around exploish — to keep the rendering path simple and avoid
 * coupling the text-mode shell to the GUI protocol.
 */

/* __EXPLOIDUS_USERSPACE__ defined by -D flag in SHELL_CFLAGS */
#include "../libc/syscall.h"
#include "../compositor/compositor.h"
#include "../compositor/gui_font.h"

/*  window / grid geometry  */
#define CELL_W      8        /* glyph cell width  = GUI_FONT_W             */
#define CELL_H      16       /* glyph cell height = GUI_FONT_H            */
#define GRID_COLS   76
#define GRID_ROWS   22
#define WIN_W       (GRID_COLS * CELL_W + 16)   /* + padding              */
#define WIN_H       (GRID_ROWS * CELL_H + 16)

#define COL_BG      0xFF0D1117u
#define COL_FG      0xFFC9D1D9u
#define COL_PROMPT  0xFF58A6FFu
#define COL_CURSOR  0xFF58A6FFu
#define COL_ERR     0xFFEF4444u

static uint32_t *g_buf;    /* SHM pixel buffer, WIN_W x WIN_H ARGB32 */

/*  character cell grid (what gets rendered)  */
static char     g_grid[GRID_ROWS][GRID_COLS];
static int      g_cur_row = 0, g_cur_col = 0;

/*  input line buffer  */
static char     g_line[256];
static int      g_line_len = 0;

/*  pixel buffer primitives  */

static void buf_fill(int x, int y, int w, int h, uint32_t col)
{
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= WIN_H) continue;
        for (int c = x; c < x + w; c++) {
            if (c < 0 || c >= WIN_W) continue;
            g_buf[row * WIN_W + c] = col;
        }
    }
}


static void buf_glyph(int x, int y, unsigned char c, uint32_t fg)
{
    /* Use the shared 8x16 font from gui_font.h */
    gui_font_char(g_buf, WIN_W, WIN_H, x, y, c, fg, 0);
}

/*  grid → pixel render  */

static void render_grid(void)
{
    buf_fill(0, 0, WIN_W, WIN_H, COL_BG);

    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            char ch = g_grid[r][c];
            if (ch == 0 || ch == ' ') continue;
            int px = 8 + c * CELL_W;
            int py = 8 + r * CELL_H;
            buf_glyph(px, py, (unsigned char)ch, COL_FG);
        }
    }

    /* Cursor block */
    int cx = 8 + g_cur_col * CELL_W;
    int cy = 8 + g_cur_row * CELL_H;
    buf_fill(cx, cy + 7, 5, 1, COL_CURSOR);
}

/*  grid manipulation  */

static void grid_scroll(void)
{
    for (int r = 0; r < GRID_ROWS - 1; r++)
        for (int c = 0; c < GRID_COLS; c++)
            g_grid[r][c] = g_grid[r + 1][c];
    for (int c = 0; c < GRID_COLS; c++)
        g_grid[GRID_ROWS - 1][c] = 0;
}

static void grid_newline(void)
{
    g_cur_col = 0;
    g_cur_row++;
    if (g_cur_row >= GRID_ROWS) {
        grid_scroll();
        g_cur_row = GRID_ROWS - 1;
    }
}

static void grid_putc(char c)
{
    if (c == '\n') { grid_newline(); return; }
    if (c == '\r') { g_cur_col = 0; return; }
    if (c == '\b') {
        if (g_cur_col > 0) { g_cur_col--; g_grid[g_cur_row][g_cur_col] = 0; }
        return;
    }
    g_grid[g_cur_row][g_cur_col] = c;
    g_cur_col++;
    if (g_cur_col >= GRID_COLS) grid_newline();
}

static void grid_puts(const char *s)
{
    while (*s) grid_putc(*s++);
}

/*  tiny string helpers (no libc here)  */

static int term_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int term_starts(const char *s, const char *prefix)
{
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

/*  built-in commands  */

static void cmd_help(void)
{
    grid_puts("Built-in commands:\n");
    grid_puts("  help    - this message\n");
    grid_puts("  clear   - clear screen\n");
    grid_puts("  echo X  - print X\n");
    grid_puts("  pwd     - print working directory\n");
    grid_puts("  ls      - list current directory\n");
    grid_puts("  exit    - close terminal window\n");
}

static void cmd_clear(void)
{
    for (int r = 0; r < GRID_ROWS; r++)
        for (int c = 0; c < GRID_COLS; c++)
            g_grid[r][c] = 0;
    g_cur_row = 0; g_cur_col = 0;
}

static void cmd_pwd(void)
{
    char buf[256];
    if (getcwd(buf, sizeof(buf)) >= 0) {
        grid_puts(buf);
        grid_putc('\n');
    } else {
        grid_puts("pwd: error\n");
    }
}

static void cmd_ls(void)
{
    int fd = open(".", O_RDONLY);
    if (fd < 0) { grid_puts("ls: cannot open .\n"); return; }
    dirent_t ents[32];
    int64_t n;
    while ((n = readdir(fd, ents, 32)) > 0) {
        for (int64_t i = 0; i < n; i++) {
            grid_puts(ents[i].name);
            grid_putc(' ');
        }
    }
    grid_putc('\n');
    close(fd);
}

static int g_should_exit = 0;

static void execute_line(const char *line)
{
    if (!line[0]) return;

    if (term_streq(line, "help")) { cmd_help(); return; }
    if (term_streq(line, "clear")) { cmd_clear(); return; }
    if (term_streq(line, "pwd")) { cmd_pwd(); return; }
    if (term_streq(line, "ls")) { cmd_ls(); return; }
    if (term_streq(line, "exit")) { g_should_exit = 1; return; }
    if (term_starts(line, "echo ")) {
        grid_puts(line + 5);
        grid_putc('\n');
        return;
    }

    grid_puts("unknown command: ");
    grid_puts(line);
    grid_putc('\n');
}

/*  prompt  */

static void draw_prompt(void)
{
    grid_puts("$ ");
}

/*  main  */

void main(void)
{
    /* 0. Find compositor */
    uint32_t comp_pid = 0;
    for (int tries = 0; tries < 20 && !comp_pid; tries++) {
        comp_pid = compositor_pid();
        if (!comp_pid) sleep_ticks(5);
    }
    if (!comp_pid) {
        write(1, "terminal: compositor not found\n", 32);
        exit(1);
    }

    /* 1. SHM pixel buffer */
    uint32_t shm_id = shm_create((uint64_t)(WIN_W * WIN_H * 4));
    if (!shm_id) { write(1, "terminal: shm_create failed\n", 29); exit(1); }

    uint32_t *mapped = (uint32_t *)shm_map(shm_id);
    if (!mapped) { write(1, "terminal: shm_map failed\n", 26); exit(1); }
    g_buf = mapped;

    /* 2. Init grid + welcome text */
    for (int r = 0; r < GRID_ROWS; r++)
        for (int c = 0; c < GRID_COLS; c++)
            g_grid[r][c] = 0;

    grid_puts("Exploidus Terminal (Phase 3)\n");
    grid_puts("Type 'help' for commands.\n\n");
    draw_prompt();
    render_grid();

    /* 3. Register window */
    if (comp_win_create(comp_pid, shm_id, 120, 60, WIN_W, WIN_H,
                        WIN_FLAG_DECORATED, "Terminal") < 0) {
        write(1, "terminal: comp_win_create failed\n", 34);
        exit(1);
    }
    comp_damage(comp_pid, shm_id, 0, 0, WIN_W, WIN_H);

    /* 4. Event loop */
    ipc_msg_t msg;
    while (!g_should_exit) {
        ipc_recv(&msg);   /* blocking */

        if (msg.type == IPC_MSG_KEY_DOWN) {
            key_msg_t *k = (key_msg_t *)msg.data;
            char c = (char)k->ascii;

            if (c == '\n' || c == '\r') {
                g_line[g_line_len] = 0;
                grid_putc('\n');
                execute_line(g_line);
                g_line_len = 0;
                if (!g_should_exit) draw_prompt();
            } else if (c == '\b' || c == 127) {
                if (g_line_len > 0) {
                    g_line_len--;
                    grid_putc('\b');
                }
            } else if (c >= 32 && c < 127 && g_line_len < 255) {
                g_line[g_line_len++] = c;
                grid_putc(c);
            }

            render_grid();
            comp_damage(comp_pid, shm_id, 0, 0, WIN_W, WIN_H);
        }
        else if (msg.type == IPC_MSG_WIN_FOCUS ||
                 msg.type == IPC_MSG_WIN_BLUR) {
            render_grid();
            comp_damage(comp_pid, shm_id, 0, 0, WIN_W, WIN_H);
        }
    }

    /* 5. Cleanup */
    comp_win_destroy(comp_pid, shm_id);
    shm_unmap(mapped, (uint64_t)(WIN_W * WIN_H * 4));
    shm_destroy(shm_id);
    exit(0);
}