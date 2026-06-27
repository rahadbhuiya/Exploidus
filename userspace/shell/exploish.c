/*
 * exploish — Exploidus Interactive Shell
 *
 * The first user-space process. Runs as PID 5.
 * Reads commands from stdin, forks+execs them.
 *
 * Built with: x86_64-elf-gcc -ffreestanding -nostdlib
 * Linked with: crt0.asm
 */

#include "../libc/syscall.h"
#include "exploish_cmds.h"


/*  Terminal I/O   */


static void print(const char *s)
{
    puts(s);
}

static void println(const char *s)
{
    puts(s);
    putc('\n');
}

static void print_int(int64_t n)
{
    if (n < 0) { putc('-'); n = -n; }
    if (n == 0) { putc('0'); return; }

    char buf[20];
    int  i = 0;
    while (n > 0) {
        buf[i++] = (char)('0' + n % 10);
        n /= 10;
    }
    for (int j = i - 1; j >= 0; j--)
        putc(buf[j]);
}

static char read_char(void)
{
    char c = 0;
    while (read(STDIN_FILENO, &c, 1) != 1) yield();
    return c;
}


/*  Line editor — reads one line from stdin into buf        */

/* ── history ── */
#define HIST_SIZE 32
#define HIST_LEN  256
static char  hist_buf[HIST_SIZE][HIST_LEN];
static int   hist_count = 0;   /* total entries added        */
static int   hist_head  = 0;   /* next slot to write (ring)  */

static void hist_push(const char *s)
{
    if (!s || !*s) return;
    /* Avoid duplicate of last entry */
    if (hist_count > 0) {
        int last = (hist_head - 1 + HIST_SIZE) % HIST_SIZE;
        int same = 1;
        const char *a = hist_buf[last], *b = s;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) same = 1; else same = 0;
        if (same) return;
    }
    int i = 0;
    while (s[i] && i < HIST_LEN - 1) { hist_buf[hist_head][i] = s[i]; i++; }
    hist_buf[hist_head][i] = '\0';
    hist_head = (hist_head + 1) % HIST_SIZE;
    if (hist_count < HIST_SIZE) hist_count++;
}

/* idx=0 → most recent, idx=1 → one before, etc. */
static const char *hist_get(int idx)
{
    if (idx < 0 || idx >= hist_count) return 0;
    int slot = (hist_head - 1 - idx + HIST_SIZE * 2) % HIST_SIZE;
    return hist_buf[slot];
}

/* ── redraw_line ──
   Redraws the entire input buffer from scratch.
   cur_col : current screen cursor column relative to editable area start
   buf/len : full input buffer and its length
   pos     : where cursor should end up after redraw
   old_len : previous visible length (to erase leftover chars)
*/
static void redraw_line(const char *buf, int len, int pos, int old_len, int cur_col)
{
    /* 1. move back to column 0 of editable area */
    for (int i = 0; i < cur_col; i++) write(STDOUT_FILENO, "\b", 1);
    /* 2. print entire new buffer */
    for (int i = 0; i < len; i++) putc(buf[i]);
    /* 3. erase leftover chars if buffer shrank */
    for (int i = len; i < old_len; i++) putc(' ');
    for (int i = len; i < old_len; i++) write(STDOUT_FILENO, "\b", 1);
    /* 4. move cursor from end back to pos */
    for (int i = len; i > pos; i--) write(STDOUT_FILENO, "\b", 1);
}

/* ── cursor blink helpers ──
   তোমার VGA driver ANSI escape বোঝে না।
   তাই cursor character সরাসরি print করি:
   cursor_on  : '_' print করে backspace দিয়ে ফিরি
   cursor_off : space print করে backspace দিয়ে ফিরি
*/
static void cursor_on(void)
{
    write(STDOUT_FILENO, "_\b", 2);
}
static void cursor_off(void)
{
    write(STDOUT_FILENO, " \b", 2);
}

static int read_line(char *buf, int max)
{
    int len  = 0;
    int pos  = 0;
    int hidx = -1;
    static char saved[HIST_LEN];
    saved[0] = '\0';
    buf[0]   = '\0';

    /* blink state */
    int      cur_visible = 0;
    cursor_on();
    cur_visible = 1;

    while (1) {
        /* ── non-blocking read ── */
        char c = 0;
        int got = (int)read(STDIN_FILENO, &c, 1);

        if (got != 1) {
            /* no key — handle blink (same method as term_buf.c) */
            uint64_t phase = (uptime() * 2) & 1;  /* toggles ~every 0.5s */
            if ((int)phase != cur_visible) {
                if (phase) { cursor_on();  cur_visible = 1; }
                else       { cursor_off(); cur_visible = 0; }
            }
            yield();
            continue;
        }

        /* got a key — hide cursor while processing, redraw after */
        if (cur_visible) { cursor_off(); cur_visible = 0; }

        /* ── ESC sequence ── */
        if (c == 0x1B) {
            char c2 = read_char();
            if (c2 != '[') goto redraw;   /* ignore unknown ESC sequences */
            char c3 = read_char();

            /* Arrow UP — older history */
            if (c3 == 'A') {
                if (hidx == -1) {
                    int j = 0;
                    while (buf[j] && j < HIST_LEN-1) { saved[j] = buf[j]; j++; }
                    saved[j] = '\0';
                }
                const char *h = hist_get(hidx + 1);
                if (h) {
                    int old_len = len;
                    int old_pos = pos;
                    hidx++;
                    len = 0;
                    while (h[len] && len < max-1) { buf[len] = h[len]; len++; }
                    buf[len] = '\0';
                    pos = len;
                    redraw_line(buf, len, pos, old_len, old_pos);
                }
                goto redraw;
            }

            /* Arrow DOWN — newer / back to live */
            if (c3 == 'B') {
                if (hidx >= 0) {
                    int old_len = len;
                    int old_pos = pos;
                    hidx--;
                    const char *h = (hidx >= 0) ? hist_get(hidx) : saved;
                    len = 0;
                    while (h[len] && len < max-1) { buf[len] = h[len]; len++; }
                    buf[len] = '\0';
                    pos = len;
                    redraw_line(buf, len, pos, old_len, old_pos);
                }
                goto redraw;
            }

            /* Arrow LEFT */
            if (c3 == 'D') {
                if (pos > 0) { pos--; write(STDOUT_FILENO, "\b", 1); }
                goto redraw;
            }

            /* Arrow RIGHT */
            if (c3 == 'C') {
                if (pos < len) { putc(buf[pos]); pos++; }
                goto redraw;
            }

            /* Home \033[H */
            if (c3 == 'H') {
                for (int i = 0; i < pos; i++) write(STDOUT_FILENO, "\b", 1);
                pos = 0;
                goto redraw;
            }

            /* End \033[F */
            if (c3 == 'F') {
                for (int i = pos; i < len; i++) putc(buf[i]);
                pos = len;
                goto redraw;
            }

            /* Delete \033[3~ */
            if (c3 == '3') {
                read_char(); /* consume '~' */
                if (pos < len) {
                    int old_len = len;
                    for (int i = pos; i < len - 1; i++) buf[i] = buf[i+1];
                    len--;
                    buf[len] = '\0';
                    redraw_line(buf, len, pos, old_len, pos);
                }
                goto redraw;
            }

            /* ignore other CSI sequences */
            goto redraw;
        }

        /*  Enter  */
        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            putc('\n');
            hist_push(buf);
            break;
        }

        /*  Backspace  */
        if (c == '\b' || c == 127) {
            if (pos > 0) {
                int old_len = len;
                for (int i = pos - 1; i < len - 1; i++) buf[i] = buf[i+1];
                len--;
                pos--;
                buf[len] = '\0';
                redraw_line(buf, len, pos, old_len, pos + 1);
            }
            goto redraw;
        }

        /*  Ctrl-A — go to start  */
        if (c == 0x01) {
            for (int i = 0; i < pos; i++) write(STDOUT_FILENO, "\b", 1);
            pos = 0;
            goto redraw;
        }

        /*  Ctrl-E — go to end  */
        if (c == 0x05) {
            for (int i = pos; i < len; i++) putc(buf[i]);
            pos = len;
            goto redraw;
        }

        /*  Ctrl-K — kill to end  */
        if (c == 0x0B) {
            int old_len = len;
            len = pos;
            buf[len] = '\0';
            for (int i = pos; i < old_len; i++) putc(' ');
            for (int i = pos; i < old_len; i++) write(STDOUT_FILENO, "\b", 1);
            goto redraw;
        }

        /*  Ctrl-C  */
        if (c == 0x03) {
            write(STDOUT_FILENO, "^C\n", 3);
            buf[0] = '\0';
            return 0;
        }

        /*  Ctrl-L — clear screen  */
        if (c == 0x0C) {
            for (int i = 0; i < 40; i++) putc('\n');
            write(STDOUT_FILENO, buf, (size_t)len);
            for (int i = len; i > pos; i--) write(STDOUT_FILENO, "\b", 1);
            goto redraw;
        }

        /* ── Printable — insert at pos ── */
        if ((unsigned char)c >= 0x20 && len < max - 1) {
            for (int i = len; i > pos; i--) buf[i] = buf[i-1];
            buf[pos] = c;
            len++;
            buf[len] = '\0';
            write(STDOUT_FILENO, buf + pos, (size_t)(len - pos));
            for (int i = len; i > pos + 1; i--) write(STDOUT_FILENO, "\b", 1);
            pos++;
        }

redraw:
        /* show cursor after every keypress, reset blink phase */
        cursor_on();
        cur_visible = 1;
    }

    buf[len] = '\0';
    return len;
}


/*  String helpers  */


static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int str_starts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static void __attribute__((unused)) str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Skip leading spaces */
static const char *skip_spaces(const char *s)
{
    while (*s == ' ') s++;
    return s;
}


/*  Built-in commands */


static void cmd_help(void)
{
    println("Exploidus Shell -- exploish v0.1");
    println("");
    println("Built-in commands:");
    println("  help            Show this message");
    println("  clear           Clear screen");
    println("  echo <text>     Print text");
    println("  pid             Print current PID");
    println("  yield           Yield CPU to scheduler");
    println("  cap             Show capability info");
    println("  uname           Print kernel info");
    println("  exit [code]     Exit shell");
    println("  shutdown        Power off system");
    println("  reboot          Restart system");
    println("  sleep           Suspend (stub)");
    println("");
    println("Package management:");
    println("  rahu install <pkg>   Install a package");
    println("  rahu remove  <pkg>   Remove a package");
    println("  rahu list            List installed packages");
    println("  rahu search  <q>     Search repository");
    println("");
    println("System:");
    println("  ps              List running processes");
    println("  audit           Print last audit entries");
    println("  cnsl-list          List all blocked IPs with TTL");
    println("  cnsl-unblock <ip>  Manually unblock an IP from CNSL");
    println("  cnsl-ttl <ip>      Seconds until IP auto-unblocks");
    println("  alien-gui       Launch graphical interface");
    println("  ys <script>     Run a Yolish script");
}

static void cmd_echo(const char *args)
{
    println(args);
}

static void cmd_pid(void)
{
    print("PID: ");
    print_int(getpid());
    putc('\n');
}

static void cmd_uname(void)
{
    println("Exploidus 0.1.0 exploidus-kernel x86_64 Reactive-Capability-Kernel");
}

static void cmd_cap(void)
{
    println("Capability tokens — current process:");
    println("  [0] CAP_RES_FILE    inode=*  READ|WRITE|EXEC|DELEGATE");
    println("  [1] CAP_RES_PROCESS pid=*    READ|SIGNAL|DELEGATE");
    println("  [2] CAP_RES_NETWORK dev=*    NET_SEND|NET_RECV");
    println("  note: tokens are BLAKE3-authenticated, non-forgeable");
}

static const char *intent_name(uint32_t i)
{
    switch (i) {
        case 0: return "COMPUTE    ";
        case 1: return "IO         ";
        case 2: return "NETWORK    ";
        case 3: return "INTERACTIVE";
        case 4: return "AUDIT      ";
        default: return "UNKNOWN    ";
    }
}
static const char *state_name(uint32_t s)
{
    switch (s) {
        case 1: return "R"; case 2: return "S";
        case 3: return "B"; case 4: return "Z";
        default: return "?";
    }
}

static void cmd_ps(void)
{
    proc_info_t procs[64];
    int64_t n = getprocs(procs, 64);
    println("  PID  PPID  INTENT        ST  TICKS");
    for (int64_t i = 0; i < n; i++) {
        print("  ");
        print_int((int64_t)procs[i].pid);
        print("    ");
        print_int((int64_t)procs[i].parent_pid);
        print("  ");
        print(intent_name(procs[i].intent));
        print("  ");
        print(state_name(procs[i].state));
        print("  ");
        print_int((int64_t)procs[i].ticks_used);
        putc('\n');
    }
}

static const char *audit_event_name(uint32_t e)
{
    switch (e) {
        case 0:  return "CAP_CREATE  ";
        case 1:  return "CAP_DENIED  ";
        case 2:  return "CAP_FORGERY ";
        case 3:  return "CAP_REVOKE  ";
        case 4:  return "CAP_DELEGATE";
        case 5:  return "SYSCALL     ";
        case 6:  return "PROC_FORK   ";
        case 7:  return "PROC_EXEC   ";
        case 8:  return "PROC_EXIT   ";
        case 11: return "FILE_OPEN   ";
        case 12: return "FILE_WRITE  ";
        case 13: return "NET_SEND    ";
        case 14: return "NET_RECV    ";
        default: return "UNKNOWN     ";
    }
}

static void print_hex(uint64_t v)
{
    char buf[17];
    int i = 15;
    buf[16] = '\0';
    for (; i >= 0; i--) {
        uint8_t nibble = (uint8_t)(v & 0xF);
        buf[i] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        v >>= 4;
    }
    /* Skip leading zeros but keep at least one digit */
    int start = 0;
    while (start < 15 && buf[start] == '0') start++;
    puts(buf + start);
}

static void cmd_audit(void)
{
    /* Use a null capability — only PID 1/2 have audit rights in kernel.
     * The audit_dump syscall validates; if denied, fallback to count. */
    cap_token_t null_cap = {0, 0};
    audit_entry_user_t entries[16];
    int64_t n = audit_dump(null_cap, entries, 16);
    if (n <= 0) {
        int64_t total = (int64_t)syscall0(SYS_AUDIT_READ);
        print("Total audit events: ");
        print_int(total);
        println(" (no read permission — hold CAP_RIGHT_AUDIT)");
        return;
    }
    println("  TS         PID  EVENT         ARG0");
    for (int64_t i = 0; i < n; i++) {
        print("  ");
        print_int((int64_t)entries[i].timestamp);
        print("  ");
        print_int((int64_t)entries[i].pid);
        print("  ");
        print(audit_event_name(entries[i].event));
        print("  0x");
        print_hex(entries[i].arg0);
        putc('\n');
    }
}


/*  rahu package manager — delegates to /bin/rahu binary  */

static void cmd_rahu(const char *args)
{
    const char *a = skip_spaces(args);
    int64_t pid;
    if (*a)
        pid = spawn_args("/bin/rahu", a);
    else
        pid = spawn("/bin/rahu");
    if (pid < 0) println("rahu: failed to spawn /bin/rahu");
    else waitpid(pid);
}

/*  Clear screen (VT100 escape)   */



static void cmd_ls(const char *path)
{
    char abspath[256];
    if (!path || !*path) {
        /* use cwd */
        if (getcwd(abspath, 256) < 0 || abspath[0] == '\0')
            abspath[0] = '/', abspath[1] = '\0';
        path = abspath;
    } else if (path[0] != '/') {
        /* relative — prepend cwd */
        char cwd[200];
        if (getcwd(cwd, 200) >= 0 && cwd[0]) {
            int clen = 0;
            while (cwd[clen]) clen++;
            abspath[0] = '\0';
            int i = 0;
            const char *s = cwd;
            while (*s && i < 200) abspath[i++] = *s++;
            if (i > 1 && abspath[i-1] != '/') abspath[i++] = '/';
            s = path;
            while (*s && i < 254) abspath[i++] = *s++;
            abspath[i] = '\0';
        } else {
            abspath[0] = '/';
            int i = 1;
            const char *s = path;
            while (*s && i < 254) abspath[i++] = *s++;
            abspath[i] = '\0';
        }
        path = abspath;
    }
    int fd = open(path, 0);
    if (fd < 0) { print("ls: cannot open: "); println(path); return; }
    dirent_t entries[64];
    int64_t n = readdir(fd, entries, 64);
    close(fd);
    if (n <= 0) { println("(empty)"); return; }
    for (int64_t i = 0; i < n; i++) {
        if (entries[i].type == 1) print("[DIR] ");
        const char *_n = entries[i].name; if (*_n && (unsigned char)*_n < 32) _n++; println(_n);
    }
}

static void cmd_touch(const char *path)
{
    if (!*path) { println("Usage: touch <file>"); return; }
    /* build absolute path */
    char ap[256];
    if (path[0] == '/') {
        int i = 0;
        while (path[i] && i < 254) { ap[i] = path[i]; i++; }
        ap[i] = '\0';
    } else {
        if (getcwd(ap, 256) < 0 || ap[0] == '\0') { ap[0] = '/'; ap[1] = '\0'; }
        int i = 0; while (ap[i]) i++;
        if (i > 1 && ap[i-1] != '/') ap[i++] = '/';
        int j = 0;
        while (path[j] && i < 254) { ap[i++] = path[j++]; }
        ap[i] = '\0';
    }
    int fd = fs_create(ap, 0);
    if (fd < 0) { print("touch: failed: "); println(ap); return; }
    close(fd);
    print("created: "); println(ap);
}

static void cmd_cat(const char *path)
{
    if (!*path) { println("Usage: cat <file>"); return; }
    /* Make absolute path */
    char abspath[256];
    if (path[0] != '/') {
        abspath[0] = '/';
        int i = 1;
        while (*path && i < 254) abspath[i++] = *path++;
        abspath[i] = '\0';
        path = abspath;
    }
    int fd = open(path, 0);
    if (fd < 0) { print("cat: not found: "); println(path); return; }
    char buf[512];
    int64_t n;
    while ((n = read(fd, buf, 511)) > 0) {
        buf[n] = 0;
        print(buf);
    }
    putc('\n');
    close(fd);
}



static void cmd_edit(const char *path)
{
    if (!path || !*path) { println("Usage: edit <file>"); return; }

    /* Read existing content */
    static char buf[4096];
    int blen = 0;
    int fd = open(path, 0);
    if (fd >= 0) {
        blen = (int)read(fd, buf, sizeof(buf)-1);
        if (blen < 0) blen = 0;
        buf[blen] = 0;
        close(fd);
    } else {
        buf[0] = 0;
    }

    /* Show current content */
    print("\n=== edit: "); print(path); println(" ===");
    println("Commands: :w save | :q quit | :wq save+quit | :help");
    println("Type text and press Enter. Empty line = new line.");
    println("─────────────────────────────────");
    if (blen > 0) { print(buf); println(""); }
    println("─────────────────────────────────");

    /* Edit loop */
    static char newbuf[4096];
    static char line[256];
    int nlen = 0;
    int running = 1;

    /* Start with existing content */
    for (int i = 0; i < blen && nlen < 4000; i++)
        newbuf[nlen++] = buf[i];

    while (running) {
        print("> ");

        /* Read line */
        int li = 0;
        char c = 0;
        while (li < 255) {
            if (read(STDIN_FILENO, &c, 1) != 1) break;
            if (c == '\n' || c == '\r') break;
            if (c == 127 || c == '\b') {
                if (li > 0) { li--; print("\b \b"); }
                continue;
            }
            line[li++] = c;
        }
        line[li] = 0;

        if (str_eq(line, ":q")) {
            running = 0;
        } else if (str_eq(line, ":w") || str_eq(line, ":wq")) {
            /* Save file */
            int wfd = open(path, 1);
            if (wfd < 0) { println("edit: cannot save"); }
            else {
                write(wfd, newbuf, nlen);
                close(wfd);
                println("Saved!");
            }
            if (str_eq(line, ":wq")) running = 0;
        } else if (str_eq(line, ":help")) {
            println(":w   = save");
            println(":q   = quit");
            println(":wq  = save and quit");
            println(":cls = clear buffer (start fresh)");
            println(":show = show current buffer");
        } else if (str_eq(line, ":cls")) {
            nlen = 0;
            println("Buffer cleared.");
        } else if (str_eq(line, ":show")) {
            println("─── Current content ───");
            newbuf[nlen] = 0;
            println(newbuf);
            println("───────────────────────");
        } else {
            /* Append line to buffer */
            for (int i = 0; line[i] && nlen < 4090; i++)
                newbuf[nlen++] = line[i];
            newbuf[nlen++] = '\n';
        }
    }
}
static void cmd_write(const char *args)
{
    /* write <file> <text> */
    if (!*args) { println("Usage: write <file> <text>"); return; }


    const char *p = args;
    while (*p && *p != ' ') p++;
    if (!*p) { println("Usage: write <file> <text>"); return; }

    /* filename copy  */
    char fname[256];
    int flen = (int)(p - args);
    if (flen >= 255) { println("write: filename too long"); return; }
    for (int i = 0; i < flen; i++) fname[i] = args[i];
    fname[flen] = '\0';

    /* absolute path */
    char abspath[256];
    if (fname[0] != '/') {
        abspath[0] = '/';
        int i = 1;
        for (int j = 0; fname[j] && i < 254; j++) abspath[i++] = fname[j];
        abspath[i] = '\0';
    } else {
        int i = 0;
        while (fname[i] && i < 254) { abspath[i] = fname[i]; i++; }
        abspath[i] = '\0';
    }

    /* text */
    const char *text = skip_spaces(p);

    /* file open or create  */
    int fd = open(abspath, O_WRONLY);
    if (fd < 0) fd = fs_create(abspath, 0);
    if (fd < 0) { print("write: cannot create: "); println(abspath); return; }

    /* type */
    int len = 0;
    while (text[len]) len++;
    write(fd, text, (size_t)len);
    write(fd, "\n", 1);
    close(fd);
    print("written to: "); println(abspath);
}



/*  helper: layered shadow  */
static void draw_shadow(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    fb_rect(x+6, y+6, w, h, rgb(2,  2,  6 ));
    fb_rect(x+4, y+4, w, h, rgb(4,  3,  12));
    fb_rect(x+2, y+2, w, h, rgb(7,  5,  18));
}

/*  helper: gradient rect (top→bottom)  */
static void grad_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint8_t r1, uint8_t g1, uint8_t b1,
                      uint8_t r2, uint8_t g2, uint8_t b2)
{
    for (uint32_t i = 0; i < h; i++) {
        uint8_t r = (uint8_t)(r1 + (int)(r2-r1) * (int)i / (int)h);
        uint8_t g = (uint8_t)(g1 + (int)(g2-g1) * (int)i / (int)h);
        uint8_t b = (uint8_t)(b1 + (int)(b2-b1) * (int)i / (int)h);
        fb_rect(x, y+i, w, 1, rgb(r, g, b));
    }
}

/*  helper: icon box (circle-based)  */
static void draw_icon(uint32_t x, uint32_t y,
                      const char *line1, const char *line2, const char *label,
                      uint32_t border, uint32_t bg, uint32_t fg1, uint32_t fg2,
                      uint32_t desk_bg)
{
    (void)bg;
    int cx = (int)x + 35;
    int cy = (int)y + 35;

    /* shadow circle */
    fb_circle(cx+3, cy+3, 28, rgb(2, 2, 8));

    /* body circle */
    fb_circle(cx, cy, 28, rgb(
        (uint8_t)(((border>>16)&0xFF) / 6),
        (uint8_t)(((border>>8 )&0xFF) / 6),
        (uint8_t)( (border     &0xFF) / 6)
    ));

    /* inner highlight ring */
    fb_circle(cx, cy, 26, rgb(
        (uint8_t)(((border>>16)&0xFF) / 4),
        (uint8_t)(((border>>8 )&0xFF) / 4),
        (uint8_t)( (border     &0xFF) / 4)
    ));

    /* accent ring */
    fb_circle(cx, cy-8, 10, rgb(
        (uint8_t)(((border>>16)&0xFF) / 2),
        (uint8_t)(((border>>8 )&0xFF) / 2),
        (uint8_t)( (border     &0xFF) / 2)
    ));

    /* icon text — centered */
    fb_str((uint32_t)(cx - 8), (uint32_t)(cy - 20), line1, fg1,
           rgb(
               (uint8_t)(((border>>16)&0xFF)/6),
               (uint8_t)(((border>>8 )&0xFF)/6),
               (uint8_t)( (border     &0xFF)/6)
           ));
    fb_str((uint32_t)(cx - 8), (uint32_t)(cy - 4),  line2, fg2,
           rgb(
               (uint8_t)(((border>>16)&0xFF)/6),
               (uint8_t)(((border>>8 )&0xFF)/6),
               (uint8_t)( (border     &0xFF)/6)
           ));

    /* label below */
    fb_str(x + 4, y + 70, label, rgb(210,220,255), desk_bg);
}

/*  helper: draw one window  */
static void draw_window(uint32_t wx, uint32_t wy, uint32_t ww, uint32_t wh,
                        const char *title,
                        uint8_t tr, uint8_t tg, uint8_t tb,
                        uint8_t tr2,uint8_t tg2,uint8_t tb2,
                        uint32_t body_col, uint32_t border_col,
                        uint32_t desk_bg)
{
    (void)desk_bg;

    /* shadow */
    draw_shadow(wx, wy, ww, wh);

    /* body — proper rounded rect */
    fb_rrect((int)wx, (int)wy, (int)ww, (int)wh, 10, body_col);

    /* title bar gradient (drawn over top of rounded body) */
    grad_rect(wx+1, wy+1, ww-2, 28, tr,tg,tb, tr2,tg2,tb2);

    /* re-round the title bar top corners */
    fb_rrect((int)wx, (int)wy, (int)ww, 30, 10,
             rgb((uint8_t)((tr+tr2)/2), (uint8_t)((tg+tg2)/2), (uint8_t)((tb+tb2)/2)));
    /* re-apply gradient on top of rounded titlebar */
    grad_rect(wx+1, wy+1, ww-2, 28, tr,tg,tb, tr2,tg2,tb2);

    /* title bar bottom separator */
    fb_rect(wx+1, wy+29, ww-2, 1, rgb(tr2/3, tg2/3, tb2/3));

    /* left accent stripe */
    fb_rrect((int)wx+6, (int)wy+7, 4, 14, 2,
             rgb((uint8_t)((tr+60<255?tr+60:255)),
                 (uint8_t)((tg+30<255?tg+30:255)),
                 (uint8_t)((tb+60<255?tb+60:255))));

    /* title text */
    fb_str(wx+16, wy+9, title, rgb(225,215,255), rgb(0,0,0));

    /* traffic light buttons — circles */
    fb_circle((int)(wx+ww-52), (int)(wy+15), 7, rgb(240,165,20));  /* min  */
    fb_circle((int)(wx+ww-34), (int)(wy+15), 7, rgb(40,200,40));   /* max  */
    fb_circle((int)(wx+ww-16), (int)(wy+15), 7, rgb(220,50,50));   /* close */

    /* outer border */
    fb_rect(wx,      wy,      ww, 1,  border_col);
    fb_rect(wx,      wy,      1,  wh, border_col);
    fb_rect(wx+ww-1, wy,      1,  wh, rgb(((border_col>>16)&0xFF)/2,
                                          ((border_col>>8 )&0xFF)/2,
                                          ( border_col     &0xFF)/2));
    fb_rect(wx,      wy+wh-1, ww, 1,  rgb(((border_col>>16)&0xFF)/3,
                                          ((border_col>>8 )&0xFF)/3,
                                          ( border_col     &0xFF)/3));
}

static void __attribute__((unused)) draw_desktop(uint32_t w, uint32_t h)
{
    uint32_t desk_bg = rgb(8,5,28);

    /*  Background: deep space gradient  */
    for (uint32_t y = 0; y < h - 40; y++) {
        uint8_t r = (uint8_t)(6  + y * 12 / h);
        uint8_t g = (uint8_t)(3  + y *  6 / h);
        uint8_t b = (uint8_t)(22 + y * 35 / h);
        fb_rect(0, y, w, 1, rgb(r, g, b));
    }

    /*  Sidebar panel  */
    grad_rect(0, 0, 100, h-40, 12,9,32, 16,11,40);
    fb_rect(99,  0, 1, h-40, rgb(70,45,130));
    fb_rect(100, 0, 1, h-40, rgb(20,10,45));

    /* ── Sidebar icons (circle style) ── */
    draw_icon(14,  45, ">_", "sh",  "Term",
              rgb(80,120,220), rgb(14,14,38),
              rgb(120,230,120), rgb(70,160,70), desk_bg);

    draw_icon(14, 155, "[]", "fs",  "Files",
              rgb(60,200,60),  rgb(12,28,12),
              rgb(130,255,130), rgb(60,180,60), desk_bg);

    draw_icon(14, 265, "cfg","sys", "Sys",
              rgb(170,80,230), rgb(28,14,38),
              rgb(215,135,255), rgb(165,80,220), desk_bg);

    /*  Main window: Terminal  */
    uint32_t wx = 108, wy = 36;
    uint32_t ww = w - 128;
    uint32_t wh = (h - 40) * 6 / 10;

    draw_window(wx, wy, ww, wh,
                "Terminal  -  exploish",
                52,22,120,  36,14,80,
                rgb(10,10,20), rgb(110,70,230), desk_bg);

    /* terminal content */
    uint32_t tx = wx+12, ty = wy+36;
    uint32_t bg = rgb(10,10,20);

    fb_str(tx, ty,
           "Exploidus v0.1.0  --  exploish",
           rgb(80,255,80), bg);
    fb_str(tx, ty+16,
           "Copyright (c) Exploidus Project. MIT License.",
           rgb(70,70,120), bg);
    fb_rect(wx+1, ty+30, ww-2, 1, rgb(30,20,60));
    fb_str(tx, ty+38,  "star@exploidus:~$ uname -a",
           rgb(80,170,255), bg);
    fb_str(tx+8, ty+54,
           "Exploidus 0.1.0 x86_64 Reactive-Capability-Kernel",
           rgb(200,200,200), bg);
    fb_str(tx, ty+74,  "star@exploidus:~$ ls /",
           rgb(80,170,255), bg);
    fb_str(tx+8, ty+90,
           "bin/   etc/   home/   lib/   tmp/   var/",
           rgb(200,200,200), bg);
    fb_str(tx, ty+110, "star@exploidus:~$ cat /etc/motd",
           rgb(80,170,255), bg);
    fb_str(tx+8, ty+126,"Welcome to Exploidus OS!",
           rgb(255,200,60), bg);
    fb_str(tx+8, ty+142,
           "A capability-based reactive microkernel.",
           rgb(170,150,255), bg);
    fb_rect(wx+1, ty+158, ww-2, 1, rgb(30,20,60));
    fb_str(tx, ty+166, "star@exploidus:~$ alien-gui",
           rgb(80,170,255), bg);
    fb_str(tx+8, ty+182,
           "Launching graphical environment...",
           rgb(80,255,80), bg);
    fb_rect(wx+1, ty+198, ww-2, 1, rgb(30,20,60));
    fb_str(tx, ty+206, "star@exploidus:~$ ",
           rgb(80,170,255), bg);
    /* cursor */
    fb_rect(tx+144, ty+206, 8, 14, rgb(100,180,255));

    /*  Second window: System Info  */
    uint32_t wx2 = wx+50, wy2 = wy + wh + 12;
    uint32_t ww2 = ww - 30;
    uint32_t wh2 = (h - 40) - wh - 20;

    if ((int)wh2 > 30) {
        draw_window(wx2, wy2, ww2, wh2,
                    "System Info",
                    18,32,18,  12,22,12,
                    rgb(8,16,8), rgb(50,180,50), desk_bg);

        uint32_t bg2 = rgb(8,16,8);
        uint32_t sx = wx2+12, sy = wy2+36;

        fb_str(sx, sy,
               "OS       Exploidus v0.1.0",         rgb(80,255,80),  bg2);
        fb_str(sx, sy+14,
               "Kernel   Reactive Capability Kernel",rgb(60,210,60),  bg2);
        fb_str(sx, sy+28,
               "Shell    exploish v0.1.0",           rgb(60,210,60),  bg2);
        fb_rect(sx, sy+42, ww2-24, 1, rgb(25,50,25));
        fb_str(sx, sy+50,
               "RAM      256 MiB",                   rgb(80,180,255), bg2);
        fb_str(sx, sy+64,
               "CPU      x86_64  qemu64",            rgb(80,180,255), bg2);
        fb_str(sx, sy+78,
               "Video    VESA 800x600x32",           rgb(80,180,255), bg2);
        fb_rect(sx, sy+92, ww2-24, 1, rgb(25,50,25));
        fb_str(sx, sy+100,
               "Capability System   Active",         rgb(190,90,255), bg2);
        fb_str(sx, sy+114,
               "Audit Ring          Online",         rgb(190,90,255), bg2);
        fb_str(sx, sy+128,
               "Intent Engine       Running",        rgb(190,90,255), bg2);
    }

    /*  Taskbar  */
    grad_rect(0, h-40, w, 40, 10,8,28, 16,12,36);
    fb_rect(0, h-41, w, 1, rgb(110,75,220));
    fb_rect(0, h-40, w, 1, rgb(55, 35,130));

    /* start button — rounded */
    fb_rrect(4, (int)(h-35), 88, 26, 8, rgb(65,42,158));
    grad_rect(5, h-34, 86, 24, 80,55,185, 58,35,148);
    fb_rect(4, h-35, 88, 1, rgb(160,130,255));
    fb_str(12, h-27, "* Exploidus", rgb(225,215,255), rgb(0,0,0));

    /* Terminal tab — active, rounded pill */
    fb_rrect(96, (int)(h-35), 128, 26, 13, rgb(38,30,80));
    grad_rect(97, h-34, 126, 24, 40,32,82, 26,20,58);
    fb_rect(96,  h-35, 128, 1, rgb(120,90,235));
    fb_rect(96,  h-10, 128, 2, rgb(130,100,245)); /* active underline */
    fb_str(114,  h-27, "> Terminal", rgb(205,190,255), rgb(0,0,0));

    /* Sys Info tab — rounded pill */
    fb_rrect(230, (int)(h-35), 118, 26, 13, rgb(14,26,14));
    grad_rect(231, h-34, 116, 24, 16,28,16, 11,20,11);
    fb_rect(230, h-35, 118, 1, rgb(55,190,55));
    fb_str(244,  h-27, "> Sys Info", rgb(160,245,160), rgb(0,0,0));

    /* clock */
    fb_str(w-72, h-27, "00:00:00", rgb(190,175,255), rgb(0,0,0));
}




/*  Window state  */
typedef struct {
    int x, y, w, h;          /* current position/size  */
    int px, py, pw, ph;      /* saved (for restore)    */
    int visible;              /* 0=closed 1=normal      */
    int minimized;            /* 1=minimized to taskbar */
    int maximized;            /* 1=fullscreen           */

    const char *title;
    uint32_t border_col;
    uint8_t  tr,tg,tb;       /* titlebar gradient top  */
    uint8_t  tr2,tg2,tb2;    /* titlebar gradient bot  */
    uint32_t body_col;
} win_t;

/* circle hit test */
static int hit_circle(int mx, int my, int cx, int cy, int r)
{
    int dx = mx-cx, dy = my-cy;
    return dx*dx + dy*dy <= r*r;
}

/* title bar hit (excluding buttons) */
static int hit_titlebar(int mx, int my, win_t *w)
{
    return mx >= w->x+2 && mx <= w->x+w->w-65
        && my >= w->y   && my <= w->y+28;
}

/*  Draw background + sidebar + taskbar  */
static void draw_bg(uint32_t W, uint32_t H, win_t *wins, int nwins)
{
    /* background gradient */
    for (uint32_t y = 0; y < H-40; y++) {
        uint8_t r=(uint8_t)(6+y*12/H);
        uint8_t g=(uint8_t)(3+y*6/H);
        uint8_t b=(uint8_t)(22+y*35/H);
        fb_rect(0,y,W,1,rgb(r,g,b));
    }



    /* taskbar */
    grad_rect(0,H-40,W,40, 10,8,28, 16,12,36);
    fb_rect(0,H-41,W,1,rgb(110,75,220));
    fb_rect(0,H-40,W,1,rgb(55,35,130));

    /* start button */
    fb_rrect(4,(int)(H-35),88,26,8,rgb(65,42,158));
    grad_rect(5,H-34,86,24, 80,55,185, 58,35,148);
    fb_rect(4,H-35,88,1,rgb(160,130,255));
    fb_str(12,H-27,"* Exploidus",rgb(225,215,255),rgb(0,0,0));

    /* taskbar tabs for each window */
    int tx=96;
    for (int i=0; i<nwins; i++) {
        win_t *wn=&wins[i];
        if (!wn->visible) continue;

        int active = !wn->minimized;
        uint32_t fill   = active ? rgb(38,30,80)  : rgb(14,14,30);
        uint32_t top    = active ? rgb(120,90,235): rgb(60,40,120);
        uint32_t txtcol = active ? rgb(205,190,255):rgb(140,130,180);
        uint32_t uline  = active ? rgb(130,100,245):rgb(0,0,0);

        fb_rrect(tx,(int)(H-35),128,26,13,fill);
        fb_rect(tx,H-35,128,1,top);
        if (active) fb_rect(tx,H-10,128,2,uline);
        fb_str((uint32_t)(tx+14),H-27,wn->title,txtcol,rgb(0,0,0));
        tx+=134;
    }

    /* clock */
    fb_str(W-72,H-27,"00:00:00",rgb(190,175,255),rgb(0,0,0));
}

/*  Draw one window  */
static void draw_win_frame(win_t *w)
{
    if (!w->visible || w->minimized) return;

    /* shadow */
    draw_shadow((uint32_t)w->x,(uint32_t)w->y,
                (uint32_t)w->w,(uint32_t)w->h);

    /* body */
    fb_rrect(w->x,w->y,w->w,w->h,10,w->body_col);

    /* title bar */
    fb_rrect(w->x,w->y,w->w,30,10,
             rgb((uint8_t)((w->tr+w->tr2)/2),
                 (uint8_t)((w->tg+w->tg2)/2),
                 (uint8_t)((w->tb+w->tb2)/2)));
    grad_rect((uint32_t)(w->x+1),(uint32_t)(w->y+1),
              (uint32_t)(w->w-2),28,
              w->tr,w->tg,w->tb, w->tr2,w->tg2,w->tb2);

    /* separator */
    fb_rect((uint32_t)(w->x+1),(uint32_t)(w->y+29),
            (uint32_t)(w->w-2),1,
            rgb(w->tr2/3,w->tg2/3,w->tb2/3));

    /* accent stripe */
    fb_rrect(w->x+6,w->y+7,4,14,2,
             rgb((uint8_t)(w->tr+60<255?w->tr+60:255),
                 (uint8_t)(w->tg+30<255?w->tg+30:255),
                 (uint8_t)(w->tb+60<255?w->tb+60:255)));

    /* title */
    fb_str((uint32_t)(w->x+16),(uint32_t)(w->y+9),
           w->title,rgb(225,215,255),rgb(0,0,0));

    /* traffic light circles */
    int bx=w->x+w->w, by=w->y+15;
    fb_circle(bx-52,by,7,rgb(240,165,20));   /* min   */
    fb_circle(bx-34,by,7,rgb(40,200,40));    /* max   */
    fb_circle(bx-16,by,7,rgb(220,50,50));    /* close */

    /* outer border */
    fb_rect((uint32_t)w->x,(uint32_t)w->y,(uint32_t)w->w,1,w->border_col);
    fb_rect((uint32_t)w->x,(uint32_t)w->y,1,(uint32_t)w->h,w->border_col);
    fb_rect((uint32_t)(w->x+w->w-1),(uint32_t)w->y,1,(uint32_t)w->h,
            rgb(((w->border_col>>16)&0xFF)/2,
                ((w->border_col>>8 )&0xFF)/2,
                ( w->border_col     &0xFF)/2));
    fb_rect((uint32_t)w->x,(uint32_t)(w->y+w->h-1),(uint32_t)w->w,1,
            rgb(((w->border_col>>16)&0xFF)/3,
                ((w->border_col>>8 )&0xFF)/3,
                ( w->border_col     &0xFF)/3));
}

/*  Terminal content  */
static void draw_terminal_content(win_t *w)
{
    if (!w->visible || w->minimized) return;
    int tx=w->x+12, ty=w->y+36;
    uint32_t bg=w->body_col;

    fb_str((uint32_t)tx,(uint32_t)ty,
           "Exploidus v0.1.0  --  exploish",rgb(80,255,80),bg);
    fb_str((uint32_t)tx,(uint32_t)(ty+16),
           "Copyright (c) Exploidus Project. MIT License.",rgb(70,70,120),bg);
    fb_rect((uint32_t)(w->x+1),(uint32_t)(ty+30),(uint32_t)(w->w-2),1,rgb(30,20,60));
    fb_str((uint32_t)tx,(uint32_t)(ty+38),
           "star@exploidus:~$ uname -a",rgb(80,170,255),bg);
    fb_str((uint32_t)(tx+8),(uint32_t)(ty+54),
           "Exploidus 0.1.0 x86_64 Reactive-Capability-Kernel",rgb(200,200,200),bg);
    fb_str((uint32_t)tx,(uint32_t)(ty+74),
           "star@exploidus:~$ ls /",rgb(80,170,255),bg);
    fb_str((uint32_t)(tx+8),(uint32_t)(ty+90),
           "bin/   etc/   home/   lib/   tmp/   var/",rgb(200,200,200),bg);
    fb_str((uint32_t)tx,(uint32_t)(ty+110),
           "star@exploidus:~$ cat /etc/motd",rgb(80,170,255),bg);
    fb_str((uint32_t)(tx+8),(uint32_t)(ty+126),
           "Welcome to Exploidus OS!",rgb(255,200,60),bg);
    fb_str((uint32_t)(tx+8),(uint32_t)(ty+142),
           "A capability-based reactive microkernel.",rgb(170,150,255),bg);
    fb_rect((uint32_t)(w->x+1),(uint32_t)(ty+158),(uint32_t)(w->w-2),1,rgb(30,20,60));
    fb_str((uint32_t)tx,(uint32_t)(ty+166),
           "star@exploidus:~$ alien-gui",rgb(80,170,255),bg);
    fb_str((uint32_t)(tx+8),(uint32_t)(ty+182),
           "Launching graphical environment...",rgb(80,255,80),bg);
    fb_rect((uint32_t)(w->x+1),(uint32_t)(ty+198),(uint32_t)(w->w-2),1,rgb(30,20,60));
    fb_str((uint32_t)tx,(uint32_t)(ty+206),
           "star@exploidus:~$ ",rgb(80,170,255),bg);
    fb_rect((uint32_t)(tx+144),(uint32_t)(ty+206),8,14,rgb(100,180,255));
}

/*  System Info content  */
static void draw_sysinfo_content(win_t *w)
{
    if (!w->visible || w->minimized) return;
    uint32_t bg2=w->body_col;
    int sx=w->x+12, sy=w->y+36;

    fb_str((uint32_t)sx,(uint32_t)sy,
           "OS       Exploidus v0.1.0",rgb(80,255,80),bg2);
    fb_str((uint32_t)sx,(uint32_t)(sy+14),
           "Kernel   Reactive Capability Kernel",rgb(60,210,60),bg2);
    fb_str((uint32_t)sx,(uint32_t)(sy+28),
           "Shell    exploish v0.1.0",rgb(60,210,60),bg2);
    fb_rect((uint32_t)sx,(uint32_t)(sy+42),(uint32_t)(w->w-24),1,rgb(25,50,25));
    fb_str((uint32_t)sx,(uint32_t)(sy+50),
           "RAM      256 MiB",rgb(80,180,255),bg2);
    fb_str((uint32_t)sx,(uint32_t)(sy+64),
           "CPU      x86_64  qemu64",rgb(80,180,255),bg2);
    fb_str((uint32_t)sx,(uint32_t)(sy+78),
           "Video    VESA 800x600x32",rgb(80,180,255),bg2);
    fb_rect((uint32_t)sx,(uint32_t)(sy+92),(uint32_t)(w->w-24),1,rgb(25,50,25));
    fb_str((uint32_t)sx,(uint32_t)(sy+100),
           "Capability System   Active",rgb(190,90,255),bg2);
    fb_str((uint32_t)sx,(uint32_t)(sy+114),
           "Audit Ring          Online",rgb(190,90,255),bg2);
    fb_str((uint32_t)sx,(uint32_t)(sy+128),
           "Intent Engine       Running",rgb(190,90,255),bg2);
}

/*  Full redraw  */
static void redraw_all(uint32_t W, uint32_t H, win_t *wins, int nwins)
{
    draw_bg(W, H, wins, nwins);
    /* back window first */
    draw_win_frame(&wins[1]);
    draw_sysinfo_content(&wins[1]);
    /* front window */
    draw_win_frame(&wins[0]);
    draw_terminal_content(&wins[0]);
}

/* cmd_gfx()  */
static void cmd_gfx(void)
{
    uint32_t W, H, active;
    if (fb_info(&W, &H, &active) < 0 || !active) {
        println("alien-gui: no framebuffer");
        return;
    }

    /*  Initial window states  */
    win_t wins[2];

    /* Terminal window */
    wins[0].x=108; wins[0].y=8;
    wins[0].w=(int)W-116; wins[0].h=((int)H-40)*6/10;
    wins[0].visible=1; wins[0].minimized=0; wins[0].maximized=0;
    wins[0].title="Terminal";
    wins[0].border_col=rgb(110,70,230);
    wins[0].tr=52;  wins[0].tg=22;  wins[0].tb=120;
    wins[0].tr2=36; wins[0].tg2=14; wins[0].tb2=80;
    wins[0].body_col=rgb(10,10,20);

    /* System Info window */
    wins[1].x=108; wins[1].y=8+wins[0].h+8;
    wins[1].w=(int)W-116; wins[1].h=((int)H-40)-wins[0].h-24;
    wins[1].visible=1; wins[1].minimized=0; wins[1].maximized=0;
    wins[1].title="Sys Info";
    wins[1].border_col=rgb(50,180,50);
    wins[1].tr=18;  wins[1].tg=32;  wins[1].tb=18;
    wins[1].tr2=12; wins[1].tg2=22; wins[1].tb2=12;
    wins[1].body_col=rgb(8,16,8);
    /* Boot splash */
    fb_clear(rgb(0,0,0));
    {
        uint32_t cx=W/2, cy=H/2;
        uint32_t pw=340, ph=160;
        uint32_t px2=cx-pw/2, py2=cy-ph/2;
        for (uint32_t i=0; i<ph; i++) {
            uint8_t r2=(uint8_t)(8+i*6/ph);
            uint8_t g2=(uint8_t)(4+i*3/ph);
            uint8_t b2=(uint8_t)(25+i*15/ph);
            fb_rect(px2,py2+i,pw,1,rgb(r2,g2,b2));
        }
        fb_rect(px2-4,py2-4,pw+8,ph+8,rgb(20,10,55));
        fb_rect(px2-2,py2-2,pw+4,ph+4,rgb(35,18,90));
        for (uint32_t i=0; i<ph; i++) {
            uint8_t r2=(uint8_t)(8+i*6/ph);
            uint8_t g2=(uint8_t)(4+i*3/ph);
            uint8_t b2=(uint8_t)(25+i*15/ph);
            fb_rect(px2,py2+i,pw,1,rgb(r2,g2,b2));
        }
        fb_rect(px2,py2,pw,2,rgb(100,60,255));
        fb_rect(px2,py2+ph-2,pw,2,rgb(40,20,120));
        fb_rect(px2,py2,2,ph,rgb(80,50,200));
        fb_rect(px2+pw-2,py2,2,ph,rgb(40,20,120));
        fb_rect(px2,py2,16,2,rgb(160,120,255));
        fb_rect(px2,py2,2,16,rgb(160,120,255));
        fb_rect(px2+pw-16,py2,16,2,rgb(160,120,255));
        fb_rect(px2+pw-2,py2,2,16,rgb(160,120,255));
        uint32_t tbg=rgb(8,4,25);
        fb_str(cx-48,py2+20,"EXPLOIDUS  OS",rgb(200,175,255),tbg);
        fb_str(cx-20,py2+36,"v 0.1.0",rgb(100,80,180),tbg);
        fb_rect(px2+16,py2+52,pw-32,1,rgb(70,45,180));
        fb_str(cx-88,py2+62,"Reactive Capability Kernel",rgb(80,170,255),tbg);
        fb_str(cx-68,py2+78,"x86_64  |  MIT License",rgb(50,50,100),tbg);
        fb_rect(px2+16,py2+96,pw-32,1,rgb(40,25,100));
        fb_str(cx-48,py2+106,"Loading desktop...",rgb(80,255,80),tbg);
        fb_rect(px2,py2+ph-20,pw,20,rgb(12,6,35));
        fb_rect(px2,py2+ph-20,pw,1,rgb(55,35,140));
        fb_str(px2+8,py2+ph-14,"exploidus.os",rgb(50,50,100),rgb(12,6,35));
        fb_flip();
        uint64_t t0=uptime();
        while(uptime()-t0 < 2) yield();
    }

    redraw_all(W, H, wins, 2);
    fb_flip();

    /*  Drag state  */
    uint64_t prev_secs=0xFFFFFFFFFFFFFFFFULL;
    int drag_win=-1;         /* which window being dragged */
    int drag_ox=0, drag_oy=0; /* offset within title bar  */
    int32_t prev_btn=0;

    for (;;) {
        int32_t mx, my, btn;
        mouse_pos(&mx, &my, &btn);

        int left_down  = (btn&1) && !(prev_btn&1);  /* press  */
        int left_held  = (btn&1);                    /* hold   */
        int left_up    = !(btn&1) && (prev_btn&1);  /* release */

        /*  Mouse press  */
        if (left_down) {
            int handled=0;

            /* check windows front→back */
            for (int i=0; i<2 && !handled; i++) {
                win_t *wn=&wins[i];
                if (!wn->visible || wn->minimized) continue;

                int bx=wn->x+wn->w, by=wn->y+15;

                /* CLOSE */
                if (hit_circle(mx,my,bx-16,by,9)) {
                    wn->visible=0;
                    redraw_all(W,H,wins,2);
                    fb_flip();
                    handled=1;
                }
                /* MINIMIZE */
                else if (hit_circle(mx,my,bx-52,by,9)) {
                    wn->minimized=1;
                    redraw_all(W,H,wins,2);
                    fb_flip();
                    handled=1;
                }
                /* MAXIMIZE / RESTORE */
                else if (hit_circle(mx,my,bx-34,by,9)) {
                    if (!wn->maximized) {
                        /* save */
                        wn->px=wn->x; wn->py=wn->y;
                        wn->pw=wn->w; wn->ph=wn->h;
                        /* fullscreen (inside sidebar+taskbar) */
                        wn->x=101; wn->y=0;
                        wn->w=(int)W-102; wn->h=(int)H-41;
                        wn->maximized=1;
                    } else {
                        /* restore */
                        wn->x=wn->px; wn->y=wn->py;
                        wn->w=wn->pw; wn->h=wn->ph;
                        wn->maximized=0;
                    }
                    redraw_all(W,H,wins,2);
                    fb_flip();
                    handled=1;
                }
                /* TITLE BAR → start drag */
                else if (hit_titlebar(mx,my,wn) && !wn->maximized) {
                    drag_win=i;
                    drag_ox=mx-wn->x;
                    drag_oy=my-wn->y;
                    handled=1;
                }
            }

            /* check sidebar icons */
            if (!handled) {
                /* Term icon */
                if (mx>=14 && mx<=84 && my>=45 && my<=125) {
                    wins[0].minimized=0;
                    wins[0].visible=1;
                    redraw_all(W,H,wins,2);
                    fb_flip();
                    handled=1;
                }
                /* Files icon */
                else if (mx>=14 && mx<=84 && my>=155 && my<=235) {
                    wins[1].minimized=0;
                    wins[1].visible=1;
                    redraw_all(W,H,wins,2);
                    fb_flip();
                    handled=1;
                }
                /* Start button */
                else if (mx>=4 && mx<=92 && my>=(int)(H-35) && my<=(int)(H-9)) {
                    wins[0].minimized=0; wins[0].visible=1;
                    wins[1].minimized=0; wins[1].visible=1;
                    redraw_all(W,H,wins,2);
                    fb_flip();
                    handled=1;
                }
            }

            /* check taskbar tabs → restore minimized */
            if (!handled) {
                int tx=96;
                for (int i=0; i<2; i++) {
                    win_t *wn=&wins[i];
                    if (!wn->visible) { tx+=134; continue; }
                    if (my>=(int)(H-35) && my<=(int)(H-10)
                        && mx>=tx && mx<=tx+128) {
                        wn->minimized=0;
                        redraw_all(W,H,wins,2);
                        fb_flip();
                        break;
                    }
                    tx+=134;
                }
            }
        }

        /*  Drag move  */
        if (left_held && drag_win>=0) {
            win_t *wn=&wins[drag_win];
            int nx=mx-drag_ox;
            int ny=my-drag_oy;
            /* clamp inside screen */
            if (nx<0) nx=0;
            if (ny<0)   ny=0;
            if (nx+wn->w>(int)W) nx=(int)W-wn->w;
            if (ny+wn->h>(int)(H-40)) ny=(int)(H-40)-wn->h;
            if (nx!=wn->x || ny!=wn->y) {
                wn->x=nx; wn->y=ny;
                redraw_all(W,H,wins,2);
                fb_flip();
            }
        }

        /*  Release  */
        if (left_up) { drag_win=-1; fb_flip(); }

        /*  Clock update (only clock area)  */
        {
            uint64_t secs=uptime();
            if (secs != prev_secs) {
                prev_secs=secs;
                uint64_t hh=secs/3600, mm=(secs%3600)/60, ss=secs%60;
                char clk[9];
                clk[0]='0'+(char)(hh/10); clk[1]='0'+(char)(hh%10);
                clk[2]=':';
                clk[3]='0'+(char)(mm/10); clk[4]='0'+(char)(mm%10);
                clk[5]=':';
                clk[6]='0'+(char)(ss/10); clk[7]='0'+(char)(ss%10);
                clk[8]=0;
                fb_rect(W-80,H-35,80,20,rgb(10,8,28));
                fb_str(W-72,H-27,clk,rgb(190,175,255),rgb(0,0,0));
                fb_flip();
            }
        }

        /* right click = exit */
        if (btn&2) break;
        prev_btn=btn;
        yield();
    }

    fb_clear(rgb(0,0,0));
    println("alien-gui: exited.");
}




static uint32_t g_rng = 0xDEADBEEF;
static uint32_t rng(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void cmd_shutdown(void)
{
    uint32_t w, h, active;
    int has_fb = (fb_info(&w, &h, &active) >= 0 && active);

    if (!has_fb) {
        println("");
        println("╔══════════════════════════════════════╗");
        println("║       EXPLOIDUS  —  SHUTDOWN         ║");
        println("║                                      ║");
        println("║   Saving state...                    ║");
        println("║   Unmounting filesystems...          ║");
        println("║   Powering off...                    ║");
        println("╚══════════════════════════════════════╝");
        println("");
        poweroff();
        return;
    }

    uint32_t navy  = rgb(0, 15, 60);
    uint32_t neon  = rgb(80, 130, 255);
    uint32_t white = rgb(220, 230, 255);
    uint32_t dim   = rgb(40, 60, 120);

    /* Phase 1 — glitch scanlines */
    for (int f = 0; f < 25; f++) {
        fb_clear(navy);
        for (int i = 0; i < 18; i++) {
            uint32_t y  = rng() % h;
            uint32_t ht = 1 + rng() % 4;
            uint8_t  r  = (uint8_t)(rng() % 60);
            uint8_t  g2 = (uint8_t)(60 + rng() % 100);
            uint8_t  b2 = (uint8_t)(150 + rng() % 105);
            fb_rect(0, y, w, ht, rgb(r, g2, b2));
        }
        /* random dots */
        for (int i = 0; i < 40; i++) {
            uint32_t x = rng() % w;
            uint32_t y = rng() % h;
            fb_pixel(x, y, neon);
        }
        sleep_ticks(2);
    }

    /* Phase 2 — fade to navy */
    fb_clear(navy);
    sleep_ticks(15);

    /* Phase 3 — big title */
    uint32_t cx = w / 2;
    uint32_t cy = h / 2;

    /* glow box behind title */
    fb_rect(cx - 120, cy - 55, 240, 110, dim);
    fb_rect(cx - 120, cy - 55, 240,   2, neon);
    fb_rect(cx - 120, cy + 53, 240,   2, neon);
    fb_rect(cx - 120, cy - 55,   2, 110, neon);
    fb_rect(cx + 118, cy - 55,   2, 110, neon);

    /* title — draw char by char for typewriter effect */
    const char *title = "EXPLOIDUS";
    uint32_t tx = cx - 36;
    for (int i = 0; title[i]; i++) {
        char buf[2] = {title[i], 0};
        fb_str(tx + (uint32_t)i * 8, cy - 38, buf, white, dim);
        sleep_ticks(3);
    }

    /* subtitle */
    sleep_ticks(8);
    fb_str(cx - 64, cy - 10, "Shutting down...", neon, dim);
    sleep_ticks(10);
    fb_str(cx - 56, cy + 14, "See you again!", rgb(150, 200, 255), dim);

    /* Phase 4 — sparkle dots around box */
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < 60; i++) {
            uint32_t x = cx - 140 + rng() % 280;
            uint32_t y = cy - 70  + rng() % 140;
            fb_pixel(x, y, neon);
        }
        sleep_ticks(10);
    }

    sleep_ticks(80);
    poweroff();
}

static void cmd_reboot(void)
{
    uint32_t w, h, active;
    int has_fb = (fb_info(&w, &h, &active) >= 0 && active);
    if (!has_fb) {
        println("");
        println("╔══════════════════════════════════════╗");
        println("║       EXPLOIDUS  —  REBOOT           ║");
        println("║                                      ║");
        println("║   Syncing...                         ║");
        println("║   Restarting system...               ║");
        println("╚══════════════════════════════════════╝");
        println("");
        reboot();
        return;
    }

    uint32_t cx = w / 2;
    uint32_t cy = h / 2;

    /* Phase 1: screen wipe */
    for (uint32_t y = 0; y < h; y += 4) {
        fb_rect(0, y, w, 4, rgb(0, 0, 0));
        if (y % 20 == 0) fb_flip();
        sleep_ticks(1);
    }
    fb_clear(rgb(0, 0, 0));
    fb_flip();
    sleep_ticks(10);

    /* Phase 2: glitch - amber/orange theme for reboot */
    for (int f = 0; f < 20; f++) {
        for (int i = 0; i < 12; i++) {
            uint32_t y  = rng() % h;
            uint32_t ht = 1 + rng() % 3;
            uint8_t  r  = (uint8_t)(180 + rng() % 75);
            uint8_t  g2 = (uint8_t)(80  + rng() % 80);
            uint8_t  b2 = (uint8_t)(rng() % 40);
            fb_rect(0, y, w, ht, rgb(r, g2, b2));
        }
        fb_flip();
        sleep_ticks(2);
        fb_clear(rgb(0, 0, 0));
    }

    /* Phase 3: center panel */
    uint32_t pw = 320, ph = 140;
    uint32_t px = cx - pw/2, py = cy - ph/2;
    uint32_t title_bg = rgb(30, 10, 5);

    /* outer glow */
    fb_rect(px-4, py-4, pw+8, ph+8, rgb(60, 20, 5));
    fb_rect(px-2, py-2, pw+4, ph+4, rgb(100, 40, 10));

    /* panel gradient */
    for (uint32_t i = 0; i < ph; i++) {
        uint8_t r = (uint8_t)(30 + i*10/ph);
        uint8_t g2= (uint8_t)(8  + i*5/ph);
        uint8_t b = (uint8_t)(2);
        fb_rect(px, py+i, pw, 1, rgb(r, g2, b));
    }

    /* border - amber theme */
    fb_rect(px,      py,      pw, 2,  rgb(255, 140, 20));
    fb_rect(px,      py+ph-2, pw, 2,  rgb(120, 60, 10));
    fb_rect(px,      py,      2,  ph, rgb(200, 100, 15));
    fb_rect(px+pw-2, py,      2,  ph, rgb(120, 60, 10));

    /* corner accents */
    fb_rect(px,       py, 12, 2, rgb(255, 180, 40));
    fb_rect(px,       py, 2, 12, rgb(255, 180, 40));
    fb_rect(px+pw-12, py, 12, 2, rgb(255, 180, 40));
    fb_rect(px+pw-2,  py, 2,  12, rgb(255, 180, 40));

    fb_flip();
    sleep_ticks(8);

    /* Phase 4: typewriter title */
    const char *title = "EXPLOIDUS OS";
    uint32_t tlen = 0;
    while (title[tlen]) tlen++;
    uint32_t tx = cx - (tlen * 8) / 2;

    for (uint32_t i = 0; i < tlen; i++) {
        char buf[2] = {title[i], 0};
        fb_str(tx + i*8 - 1, py+22, buf, rgb(180, 80, 10), title_bg);
        fb_str(tx + i*8 + 1, py+22, buf, rgb(180, 80, 10), title_bg);
        fb_str(tx + i*8,     py+22, buf, rgb(255, 200, 80), title_bg);
        fb_flip();
        sleep_ticks(4);
    }

    /* divider */
    sleep_ticks(5);
    for (uint32_t i = 0; i < pw-20; i++) {
        fb_rect(px+10+i, py+46, 1, 1, rgb(200, 100, 20));
        if (i % 20 == 0) fb_flip();
        sleep_ticks(1);
    }
    fb_flip();

    /* Phase 5: status messages */
    sleep_ticks(8);
    fb_str(cx-68, py+58, "Saving system state...",   rgb(100, 200, 100), title_bg);
    fb_flip();
    sleep_ticks(25);

    fb_str(cx-72, py+74, "Restarting services...",   rgb(255, 160, 40),  title_bg);
    fb_flip();
    sleep_ticks(20);

    fb_str(cx-56, py+90, "Rebooting now...",         rgb(255, 120, 20),  title_bg);
    fb_flip();
    sleep_ticks(15);

    fb_str(cx-52, py+112, "See you soon!", rgb(80, 80, 80), title_bg);
    fb_flip();

    /* Phase 6: wipe out */
    sleep_ticks(40);
    for (uint32_t y = 0; y < h; y += 2) {
        fb_rect(0, y,   w, 1, rgb(0,0,0));
        fb_rect(0, h-y, w, 1, rgb(0,0,0));
        if (y % 16 == 0) fb_flip();
        sleep_ticks(1);
    }
    fb_clear(rgb(0,0,0));
    fb_flip();
    sleep_ticks(20);

    reboot();
}

static void cmd_sleep(const char *args)
{
    int secs = 0;
    while (*args >= '0' && *args <= '9')
        secs = secs * 10 + (*args++ - '0');
    if (secs <= 0) { println("Usage: sleep <seconds>"); return; }

    uint32_t w, h, active;
    int has_fb = (fb_info(&w, &h, &active) >= 0 && active);
    if (!has_fb) {
        print("Sleeping for ");
        print_int(secs);
        println(" seconds...");
        sleep_ticks((uint64_t)secs * 100);
        println("Done.");
        return;
    }

    uint32_t cx = w/2, cy = h/2;
    uint32_t bw = 300, bh = 20;
    uint32_t bx = cx - bw/2, by = cy - bh/2;
    uint32_t panel_bg = rgb(8,5,25);

    /* panel */
    fb_rect(cx-160, cy-50, 320, 100, panel_bg);
    fb_rect(cx-160, cy-50, 320, 2,   rgb(80,50,200));
    fb_rect(cx-160, cy+48, 320, 2,   rgb(40,20,100));
    fb_rect(cx-160, cy-50, 2,   100, rgb(80,50,200));
    fb_rect(cx+158, cy-50, 2,   100, rgb(40,20,100));

    /* title */
    fb_str(cx-28, cy-38, "Sleeping", rgb(180,160,255), panel_bg);

    /* progress bar background */
    fb_rect(bx-2, by-2, bw+4, bh+4, rgb(20,15,50));
    fb_rect(bx,   by,   bw,   bh,   rgb(15,10,35));

    /* count text area */
    fb_str(cx-20, cy+18, "0 / 0 sec", rgb(60,60,100), panel_bg);
    fb_flip();

    for (int i = 0; i <= secs; i++) {
        /* fill progress */
        uint32_t filled = (uint32_t)((uint64_t)i * bw / (uint64_t)secs);
        fb_rect(bx, by, filled, bh, rgb(80,50,200));

        /* gradient effect on filled part */
        for (uint32_t gx = 0; gx < filled; gx++) {
            uint8_t r = (uint8_t)(60  + gx*60/bw);
            uint8_t g = (uint8_t)(30  + gx*30/bw);
            uint8_t b = (uint8_t)(180 + gx*40/bw);
            fb_rect(bx+gx, by, 1, bh, rgb(r,g,b));
        }

        /* shine line on top */
        if (filled > 0)
            fb_rect(bx, by, filled, 2, rgb(140,100,255));

        /* count */
        fb_rect(cx-30, cy+18, 80, 14, panel_bg);
        print_int(i);
        fb_str(cx-20, cy+18, " sec / ", rgb(120,100,200), panel_bg);

        /* done check */
        if (i == secs) {
            fb_rect(cx-160, cy-50, 320, 100, panel_bg);
            fb_str(cx-20, cy-8, "Done!", rgb(80,255,80), panel_bg);
            fb_flip();
            sleep_ticks(80);
            break;
        }

        fb_flip();
        sleep_ticks(100);
    }
    println("");
}






static void cmd_ping(const char *arg)
{
    if (!*arg) { println("Usage: ping <a.b.c.d>"); return; }
    uint32_t ip = 0;
    const char *p = arg;
    for (int oct = 0; oct < 4; oct++) {
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') { v = v*10 + (uint32_t)(*p - '0'); p++; }
        if (oct < 3 && *p == '.') p++;
        ip = (ip << 8) | (v & 0xFF);
    }
    print("PING "); print(arg); print(" ... ");
    int64_t r = ping(ip);
    if (r == 0) println("OK! Reply received.");
    else println("Request timeout.");
}








static void cmd_clear(void)
{
    /* VGA console ANSI বোঝে না — অনেক newline দিয়ে scroll করি */
    for (int i = 0; i < 40; i++) putc('\n');
}



/*  cnsl-unblock / cnsl-status  */
static uint32_t parse_ip4(const char *s)
{
    uint32_t a=0,b=0,c=0,d=0;
    int n=0;
    while(*s){
        if(*s>='0'&&*s<='9'){
            switch(n){
            case 0: a=a*10+(*s-'0'); break;
            case 1: b=b*10+(*s-'0'); break;
            case 2: c=c*10+(*s-'0'); break;
            case 3: d=d*10+(*s-'0'); break;
            }
        } else if(*s=='.') { n++; }
        s++;
    }
    /* network byte order: a.b.c.d → 0xAABBCCDD stored big-endian */
    return (uint32_t)((a<<24)|(b<<16)|(c<<8)|d);
}

static void cmd_cnsl(const char *args)
{
    const char *p = args;
    /* skip leading spaces already done by dispatch */

    /* cnsl-status — show all blocked IPs (no args) */
    if (!*p) {
        /* iterate — we have no list syscall yet; just print a hint */
        println("cnsl: use 'cnsl-unblock <ip>' to manually unblock an IP");
        println("cnsl: blocked IPs auto-expire after 1800 seconds (30 min)");
        return;
    }

    /* cnsl-unblock <ip> */
    if (str_starts(p, "unblock ") || str_starts(p, "unblock")) {
        const char *ip_str = skip_spaces(p + 7);
        if (!*ip_str) { println("usage: cnsl-unblock <ip>"); return; }

        uint32_t ip = parse_ip4(ip_str);

        /* show TTL before unblocking */
        uint64_t ttl = cnsl_block_ttl(ip);
        if (ttl == 0) {
            print(ip_str);
            println(" is not currently blocked");
            return;
        }

        print("Unblocking "); print(ip_str);
        print(" (had ");
        print_int((int64_t)ttl);
        println("s remaining)...");

        int r = cnsl_unblock_ip(ip);
        if (r == 0) println("Done.");
        else        println("Failed (kernel rejected).");
        return;
    }

    /* cnsl-ttl <ip> */
    if (str_starts(p, "ttl ")) {
        const char *ip_str = skip_spaces(p + 4);
        uint32_t ip  = parse_ip4(ip_str);
        uint64_t ttl = cnsl_block_ttl(ip);
        if (ttl == 0) {
            print(ip_str); println(": not blocked");
        } else {
            print(ip_str); print(": auto-unblock in ");
            print_int((int64_t)ttl); println("s");
        }
        return;
    }

    println("cnsl: unknown subcommand. try: cnsl-unblock <ip>  |  cnsl-ttl <ip>");
}


/*  cnsl-list  */
static void cmd_cnsl_list(void)
{
    cnsl_list_entry_t entries[32];
    int n = cnsl_list_ips(entries, 32);
    if (n <= 0) {
        println("No IPs currently blocked.");
        return;
    }
    println("Blocked IPs:");
    println("  IP               TTL (sec)");
    println("  ─────────────────────────");
    for (int i = 0; i < n; i++) {
        uint32_t ip = entries[i].ip;
        uint8_t  a  = (uint8_t)(ip >> 24);
        uint8_t  b  = (uint8_t)(ip >> 16);
        uint8_t  c  = (uint8_t)(ip >>  8);
        uint8_t  d  = (uint8_t)(ip      );
        print("  ");
        print_int(a); print("."); print_int(b); print(".");
        print_int(c); print("."); print_int(d);
        print("  →  ");
        print_int((int64_t)entries[i].ttl_secs);
        println("s");
    }
}

/*  Command dispatcher */


static void dispatch(const char *line)
{
    const char *l = skip_spaces(line);
    if (!*l) return;

    if (str_eq(l, "help")) {
        cmd_help();
    } else if (str_eq(l, "shutdown") || str_eq(l, "poweroff")) {
        cmd_shutdown();
    } else if (str_eq(l, "reboot")) {
        cmd_reboot();
    } else if (str_starts(l, "sleep")) {
        cmd_sleep(skip_spaces(l + 5));
    } else if (str_eq(l, "clear")) {
        cmd_clear();
    } else if (str_starts(l, "echo ")) {
        cmd_echo(skip_spaces(l + 5));
    } else if (str_eq(l, "echo")) {
        putc('\n');
    } else if (str_eq(l, "pid")) {
        cmd_pid();
    } else if (str_eq(l, "yield")) {
        yield();
    } else if (str_eq(l, "uname")) {
        cmd_uname();
    } else if (str_eq(l, "cap")) {
        cmd_cap();
    } else if (str_eq(l, "ps")) {
        cmd_ps();
    } else if (str_eq(l, "audit")) {
        cmd_audit();
    } else if (str_starts(l, "cnsl-unblock")) {
        cmd_cnsl(skip_spaces(l + 12));
    } else if (str_starts(l, "cnsl-ttl")) {
        cmd_cnsl(skip_spaces(l + 8));
    } else if (str_starts(l, "cnsl")) {
        cmd_cnsl(skip_spaces(l + 4));
    } else if (str_starts(l, "ls")) {
        cmd_ls(skip_spaces(l + 2));
    } else if (str_starts(l, "touch ")) {
        cmd_touch(skip_spaces(l + 6));
    } else if (str_starts(l, "cat ")) {
        cmd_cat(skip_spaces(l + 4));
    } else if (str_starts(l, "edit ")) {
        cmd_edit(skip_spaces(l + 5));
    } else if (str_starts(l, "write ")) {
        cmd_write(skip_spaces(l + 6));   
    } else if (str_eq(l, "mousetest")) {
        int32_t mx, my, btn;
        mouse_pos(&mx, &my, &btn);
        print("mouse x="); print_int(mx);
        print(" y="); print_int(my);
        print(" btn="); print_int(btn);
        println("");
    } else if (str_starts(l, "cd")) {
        cmd_ext_cd(skip_spaces(l + 2));
    } else if (str_eq(l, "pwd")) {
        cmd_ext_pwd();
    } else if (str_eq(l, "whoami")) {
        cmd_ext_whoami();
    } else if (str_eq(l, "hostname")) {
        cmd_ext_hostname();
    } else if (str_starts(l, "mkdir ")) {
        cmd_ext_mkdir(skip_spaces(l + 6));
    } else if (str_starts(l, "rm ")) {
        cmd_ext_rm(skip_spaces(l + 3));
    } else if (str_eq(l, "free")) {
        cmd_ext_free();
    } else if (str_eq(l, "uptime")) {
        cmd_ext_uptime();
    } else if (str_starts(l, "kill")) {
        cmd_ext_kill(skip_spaces(l + 4));
    } else if (str_eq(l, "ifconfig")) {
        cmd_ext_ifconfig();
    } else if (str_starts(l, "wc ")) {
        cmd_ext_wc(skip_spaces(l + 3));
    } else if (str_starts(l, "grep")) {
        cmd_ext_grep(skip_spaces(l + 4));
    } else if (str_starts(l, "head")) {
        cmd_ext_head(skip_spaces(l + 4));
    } else if (str_starts(l, "xxd")) {
        cmd_ext_xxd(skip_spaces(l + 3));
    /*  new Unix commands  */
    } else if (str_starts(l, "cat")) {
        cmd_ext_cat(skip_spaces(l + 3));
    } else if (str_starts(l, "touch")) {
        cmd_ext_touch(skip_spaces(l + 5));
    } else if (str_starts(l, "cp ")) {
        cmd_ext_cp(skip_spaces(l + 3));
    } else if (str_starts(l, "mv ")) {
        cmd_ext_mv(skip_spaces(l + 3));
    } else if (str_starts(l, "tail")) {
        cmd_ext_tail(skip_spaces(l + 4));
    } else if (str_starts(l, "find")) {
        cmd_ext_find(skip_spaces(l + 4));
    } else if (str_starts(l, "sed ")) {
        cmd_ext_sed(skip_spaces(l + 4));
    } else if (str_starts(l, "chmod")) {
        cmd_ext_chmod(skip_spaces(l + 5));
    } else if (str_eq(l, "env")) {
        cmd_ext_env();
    } else if (str_eq(l, "clear")) {
        cmd_ext_clear();
    } else if (str_starts(l, "tee ")) {
        cmd_ext_tee(skip_spaces(l + 4));
    } else if (str_starts(l, "cmp ")) {
        cmd_ext_cmp(skip_spaces(l + 4));
    } else if (str_starts(l, "ping ")) {
        cmd_ping(skip_spaces(l + 5));
    } else if (str_eq(l, "alien-gui")) {
        cmd_gfx();
    } else if (str_starts(l, "rahu")) {
        cmd_rahu(skip_spaces(l + 4));
    } else if (str_eq(l, "cnsl-list")) {
        cmd_cnsl_list();
    } else if (str_starts(l, "exit")) {
        const char *arg = skip_spaces(l + 4);
        int code = 0;
        while (*arg >= '0' && *arg <= '9') {
            code = code * 10 + (*arg - '0');
            arg++;
        }
        exit(code);
    } else if (str_starts(l, "ys ")) {
        const char *sc = skip_spaces(l + 3);
        static char ysp[260];
        if (sc[0]=='/'){int i=0;while(sc[i]&&i<258){ysp[i]=sc[i];i++;}ysp[i]=0;}
        else{ysp[0]='/';int i=1;while(*sc&&i<258){ysp[i++]=*sc++;}ysp[i]=0;}
        static char wa[300];
        wa[0]='/';wa[1]='y';wa[2]='s';wa[3]='_';wa[4]='r';
        wa[5]='u';wa[6]='n';wa[7]=' ';
        int i=8,j=0;
        while(ysp[j]&&i<298){wa[i++]=ysp[j++];}wa[i]=0;
        cmd_write(wa);
        int64_t pid=spawn("/bin/ys");
        if(pid<0)pid=spawn("/ys");
        if(pid<0)println("ys: not found");
        else waitpid(pid);
    } else {
            /* Try to run as program — check /bin/ first, then absolute */
            char path[260];
            int i = 0;
            const char *s = l;
            while (*s && *s != ' ' && i < 257) path[i++] = *s++;
            path[i] = '\0';

            /* If not absolute path, try /bin/<cmd> first */
            char binpath[270];
            int64_t pid = -1;

            if (path[0] != '/') {
                binpath[0] = '/'; binpath[1] = 'b'; binpath[2] = 'i';
                binpath[3] = 'n'; binpath[4] = '/';
                int j = 5, k = 0;
                while (path[k] && j < 268) binpath[j++] = path[k++];
                binpath[j] = '\0';
                pid = spawn(binpath);
            }

            /* fallback: try as-is with / prefix */
            if (pid < 0) {
                char abspath[260];
                abspath[0] = '/';
                int j = 1, k = 0;
                while (path[k] && j < 258) abspath[j++] = path[k++];
                abspath[j] = '\0';
                pid = spawn(abspath);
            }

            if (pid < 0) {
                print("exploish: command not found: ");
                println(l);
                println("Type 'help' for available commands.");
            } else {
                waitpid(pid);
            }
        }
}


/*  main    */


int main(void)
{
    char line[256];

    println("");
    println("Exploidus v0.1.0 -- exploish (Exploidus Interactive Shell)");
    println("Type 'help' for commands. Type 'exit' to quit.");
    println("");

    for (;;) {
        /* Multi-line prompt:
         *   ╭─[star@exploidus]─[<cwd>]
         *   ╰─#
         */
        char cwd[256];
        if (getcwd(cwd, 256) < 0 || cwd[0] == '\0') {
            cwd[0] = '/'; cwd[1] = '\0';
        }
        print("\xe2\x95\xad\xe2\x94\x80[star@exploidus]\xe2\x94\x80[");
        print(cwd);
        print("]\n\xe2\x95\xb0\xe2\x94\x80# ");

        /* Read command */
        int n = read_line(line, (int)sizeof(line));
        if (n < 0) break;

        dispatch(line);
    }

    return 0;
}