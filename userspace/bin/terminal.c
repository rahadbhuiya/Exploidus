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
    grid_puts("Commands:\n");
    grid_puts("  help            this message\n");
    grid_puts("  clear           clear screen\n");
    grid_puts("  echo <text>     print text\n");
    grid_puts("  pwd             working directory\n");
    grid_puts("  cd <dir>        change directory\n");
    grid_puts("  ls [dir]        list directory\n");
    grid_puts("  cat <file>      print file\n");
    grid_puts("  mkdir <dir>     create directory\n");
    grid_puts("  rm <file>       remove file\n");
    grid_puts("  open <app>      launch GUI app\n");
    grid_puts("  ps              list processes\n");
    grid_puts("  uptime          system uptime\n");
    grid_puts("  exit            close terminal\n");
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
        grid_puts(buf); grid_putc('\n');
    } else {
        grid_puts("pwd: error\n");
    }
}

static void cmd_cd(const char *path)
{
    if (!path || !*path) path = "/";
    if (chdir(path) < 0) {
        grid_puts("cd: cannot cd to: "); grid_puts(path); grid_putc('\n');
    }
}

static void cmd_ls(const char *path)
{
    if (!path || !*path) path = ".";
    int fd = open(path, O_RDONLY);
    if (fd < 0) { grid_puts("ls: cannot open: "); grid_puts(path); grid_putc('\n'); return; }
    dirent_t ents[32]; int64_t n; int col = 0;
    while ((n = readdir(fd, ents, 32)) > 0) {
        for (int64_t i = 0; i < n; i++) {
            grid_puts(ents[i].name);
            /* padding to 16 chars for alignment */
            int len = 0; const char *p = ents[i].name; while(*p++) len++;
            for (int s = len; s < 16; s++) grid_putc(' ');
            col++;
            if (col % 4 == 0) grid_putc('\n');
        }
    }
    if (col % 4 != 0) grid_putc('\n');
    close(fd);
}

static void cmd_cat(const char *path)
{
    if (!path || !*path) { grid_puts("cat: missing file\n"); return; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { grid_puts("cat: cannot open: "); grid_puts(path); grid_putc('\n'); return; }
    char buf[512]; int64_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        grid_puts(buf);
    }
    close(fd);
    grid_putc('\n');
}

static void cmd_mkdir(const char *path)
{
    if (!path || !*path) { grid_puts("mkdir: missing name\n"); return; }
    if (fs_create(path, 1) < 0) {
        grid_puts("mkdir: failed: "); grid_puts(path); grid_putc('\n');
    }
}

static void cmd_rm(const char *path)
{
    if (!path || !*path) { grid_puts("rm: missing file\n"); return; }
    if (unlink(path) < 0) {
        grid_puts("rm: failed: "); grid_puts(path); grid_putc('\n');
    }
}

static void cmd_open(const char *app)
{
    if (!app || !*app) { grid_puts("open: missing app path\n"); return; }
    int64_t pid = spawn(app);
    if (pid < 0) {
        grid_puts("open: failed: "); grid_puts(app); grid_putc('\n');
    } else {
        grid_puts("open: launched "); grid_puts(app); grid_putc('\n');
    }
}

static void cmd_ps(void)
{
    proc_info_t procs[16]; int64_t n = getprocs(procs, 16);
    if (n <= 0) { grid_puts("ps: error\n"); return; }
    grid_puts("PID  STATE  TICKS\n");
    for (int64_t i = 0; i < n; i++) {
        /* print pid */
        char nb[8]; int ni = 0; uint32_t tmp = procs[i].pid;
        if (!tmp) nb[ni++]='0';
        else { while(tmp){nb[ni++]='0'+tmp%10;tmp/=10;} }
        for(int a=0,b=ni-1;a<b;a++,b--){char t=nb[a];nb[a]=nb[b];nb[b]=t;}
        nb[ni]=0; grid_puts(nb);
        grid_puts("    ");
        const char *st = procs[i].state==1?"RUN":
                         procs[i].state==2?"RDY":
                         procs[i].state==3?"BLK":"ZMB";
        grid_puts(st); grid_putc('\n');
    }
}

static void cmd_uptime(void)
{
    uint64_t secs = uptime();
    char nb[16]; int ni = 0;
    if (!secs) nb[ni++]='0';
    else { uint64_t t=secs; while(t){nb[ni++]='0'+t%10;t/=10;} }
    for(int a=0,b=ni-1;a<b;a++,b--){char t=nb[a];nb[a]=nb[b];nb[b]=t;}
    nb[ni]=0;
    grid_puts("uptime: "); grid_puts(nb); grid_puts(" seconds\n");
}

static int g_should_exit = 0;

/*  skip leading spaces  */
static const char *term_skip(const char *s)
{
    while (*s == ' ') s++;
    return s;
}

static void execute_line(const char *line)
{
    if (!line[0]) return;

    if (term_streq(line, "help"))   { cmd_help(); return; }
    if (term_streq(line, "clear"))  { cmd_clear(); return; }
    if (term_streq(line, "pwd"))    { cmd_pwd(); return; }
    if (term_streq(line, "ps"))     { cmd_ps(); return; }
    if (term_streq(line, "uptime")) { cmd_uptime(); return; }
    if (term_streq(line, "exit"))   { g_should_exit = 1; return; }
    if (term_streq(line, "ls"))     { cmd_ls("."); return; }

    if (term_starts(line, "echo "))  { grid_puts(line + 5); grid_putc('\n'); return; }
    if (term_starts(line, "cd "))    { cmd_cd(term_skip(line + 3)); return; }
    if (term_starts(line, "ls "))    { cmd_ls(term_skip(line + 3)); return; }
    if (term_starts(line, "cat "))   { cmd_cat(term_skip(line + 4)); return; }
    if (term_starts(line, "mkdir ")) { cmd_mkdir(term_skip(line + 6)); return; }
    if (term_starts(line, "rm "))    { cmd_rm(term_skip(line + 3)); return; }
    if (term_starts(line, "open "))  { cmd_open(term_skip(line + 5)); return; }

    grid_puts("unknown: "); grid_puts(line); grid_putc('\n');
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