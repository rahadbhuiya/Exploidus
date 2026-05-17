/*
 * exploish_cmds.c - extra shell commands
 */
#include "../libc/syscall.h"

static void _print(const char *s) { puts(s); }
static void _println(const char *s) { puts(s); putc('\n'); }
static void _print_int(int64_t n) {
    if (n < 0) { putc('-'); n = -n; }
    if (n == 0) { putc('0'); return; }
    char buf[20]; int i = 0;
    while (n > 0) { buf[i++] = (char)('0' + n % 10); n /= 10; }
    for (int j = i-1; j >= 0; j--) putc(buf[j]);
}
static const char *_skip(const char *s) { while (*s == ' ') s++; return s; }

static void _abs(const char *path, char *out, int max) {
    if (path[0] == '/') {
        int i = 0;
        while (*path && i < max-1) out[i++] = *path++;
        out[i] = 0;
    } else {
        out[0] = '/'; int i = 1;
        while (*path && i < max-1) out[i++] = *path++;
        out[i] = 0;
    }
}

void cmd_ext_cd(const char *path) {
    if (!*path) path = "/";
    char ap[256];
    _abs(path, ap, 256);
    if (chdir(ap) < 0) { _print("cd: no such directory: "); _println(ap); }
}

void cmd_ext_pwd(void) {
    char buf[256];
    if (getcwd(buf, 256) == 0) _println(buf);
    else _println("/");
}

void cmd_ext_whoami(void) { _println("root"); }

void cmd_ext_hostname(void) { _println("exploidus"); }

void cmd_ext_mkdir(const char *path) {
    if (!*path) { _println("Usage: mkdir <dir>"); return; }
    char ap[256]; _abs(path, ap, 256);
    int fd = fs_create(ap, 1);
    if (fd < 0) { _print("mkdir: failed: "); _println(ap); return; }
    close(fd);
    _print("created: "); _println(ap);
}

void cmd_ext_rm(const char *path) {
    if (!*path) { _println("Usage: rm <file>"); return; }
    _println("rm: not yet implemented");
}

void cmd_ext_free(void) {
    _println("              total        used        free");
    _println("Mem:          262144      --          --");
}

void cmd_ext_uptime(void) {
    uint64_t secs = uptime();
    uint64_t hh = secs/3600, mm = (secs%3600)/60, ss = secs%60;
    _print("up ");
    _print_int((int64_t)hh); _print("h ");
    _print_int((int64_t)mm); _print("m ");
    _print_int((int64_t)ss); _println("s");
}

void cmd_ext_kill(const char *args) {
    if (!*args) { _println("Usage: kill <pid>"); return; }
    int64_t pid = 0;
    while (*args >= '0' && *args <= '9') pid = pid*10 + (*args++ - '0');
    _print("kill: sent signal to PID "); _print_int(pid); _println(" (stub)");
}

void cmd_ext_ifconfig(void) {
    _println("lo        Link encap:Local Loopback");
    _println("          inet addr:127.0.0.1");
    _println("");
    _println("eth0      Link encap:Ethernet");
    _println("          inet addr:10.0.2.15");
    _println("          Mask:255.255.255.0");
}

void cmd_ext_wc(const char *path) {
    if (!*path) { _println("Usage: wc <file>"); return; }
    char ap[256]; _abs(path, ap, 256);
    int fd = open(ap, 0);
    if (fd < 0) { _print("wc: not found: "); _println(ap); return; }
    char buf[512]; int64_t n;
    int64_t lines=0, words=0, bytes=0, in_word=0;
    while ((n = read(fd, buf, 511)) > 0) {
        bytes += n;
        for (int64_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == 10) lines++;
            if (c == 32 || c == 9 || c == 10) { in_word = 0; }
            else if (!in_word) { in_word = 1; words++; }
        }
    }
    close(fd);
    _print_int(lines); putc(9);
    _print_int(words); putc(9);
    _print_int(bytes); putc(9);
    _println(path);
}

void cmd_ext_grep(const char *args) {
    const char *p = args;
    while (*p && *p != ' ') p++;
    if (!*p) { _println("Usage: grep <pattern> <file>"); return; }
    char pat[128];
    int plen = (int)(p - args);
    if (plen > 127) plen = 127;
    for (int i = 0; i < plen; i++) pat[i] = args[i];
    pat[plen] = 0;
    const char *fpath = _skip(p);
    char ap[256]; _abs(fpath, ap, 256);
    int fd = open(ap, 0);
    if (fd < 0) { _print("grep: not found: "); _println(ap); return; }
    char line[512]; int li = 0;
    char ch[1]; int64_t n;
    while ((n = read(fd, ch, 1)) > 0) {
        if (ch[0] == 10 || li >= 511) {
            line[li] = 0;
            int found = 0;
            for (int i = 0; line[i] && !found; i++) {
                int match = 1;
                for (int j = 0; pat[j] && match; j++)
                    if (line[i+j] != pat[j]) match = 0;
                if (match) found = 1;
            }
            if (found) _println(line);
            li = 0;
        } else { line[li++] = ch[0]; }
    }
    close(fd);
}

void cmd_ext_head(const char *args) {
    int lines = 10;
    const char *path = args;
    if (args[0] == '-' && args[1] == 'n') {
        path = _skip(args + 2);
        lines = 0;
        while (*path >= '0' && *path <= '9') lines = lines*10 + (*path++ - '0');
        path = _skip(path);
    }
    if (!*path) { _println("Usage: head [-n num] <file>"); return; }
    char ap[256]; _abs(path, ap, 256);
    int fd = open(ap, 0);
    if (fd < 0) { _print("head: not found: "); _println(ap); return; }
    char buf[512]; int64_t n; int count = 0;
    while (count < lines && (n = read(fd, buf, 511)) > 0) {
        for (int64_t i = 0; i < n && count < lines; i++) {
            putc(buf[i]);
            if (buf[i] == 10) count++;
        }
    }
    close(fd);
}

void cmd_ext_xxd(const char *path) {
    if (!*path) { _println("Usage: xxd <file>"); return; }
    char ap[256]; _abs(path, ap, 256);
    int fd = open(ap, 0);
    if (fd < 0) { _print("xxd: not found: "); _println(ap); return; }
    uint8_t buf[16]; int64_t n; uint64_t offset = 0;
    while ((n = read(fd, buf, 16)) > 0) {
        char hx[9]; uint64_t v = offset;
        for (int i = 7; i >= 0; i--) {
            uint8_t nb = (uint8_t)(v & 0xF);
            hx[i] = (char)(nb < 10 ? '0'+nb : 'a'+nb-10);
            v >>= 4;
        }
        hx[8] = 0;
        puts(hx); puts(": ");
        for (int64_t i = 0; i < n; i++) {
            uint8_t hi = (buf[i] >> 4) & 0xF;
            uint8_t lo = buf[i] & 0xF;
            putc((char)(hi < 10 ? '0'+hi : 'a'+hi-10));
            putc((char)(lo < 10 ? '0'+lo : 'a'+lo-10));
            putc(' ');
            if (i == 7) putc(' ');
        }
        puts(" |");
        for (int64_t i = 0; i < n; i++)
            putc(buf[i] >= 32 && buf[i] < 127 ? (char)buf[i] : '.');
        _println("|");
        offset += (uint64_t)n;
    }
    close(fd);
}
