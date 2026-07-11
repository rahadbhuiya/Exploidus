/*
 * Exploidus minimal stdio.c
 */
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "stdarg.h"
#include "syscall.h"

/*  FILE objects for stdin/stdout/stderr  */
static FILE _stdin  = { 0, 0, 0, -1 };
static FILE _stdout = { 1, 0, 0, -1 };
static FILE _stderr = { 2, 0, 0, -1 };
FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

/*  Low level  */
int fputc(int c, FILE *f)
{
    char ch = (char)c;
    write(f->fd, &ch, 1);
    return c;
}

int putchar(int c) { return fputc(c, stdout); }

int fputs(const char *s, FILE *f)
{
    size_t n = strlen(s);
    write(f->fd, s, n);
    return (int)n;
}

int puts(const char *s)
{
    fputs(s, stdout);
    fputc('\n', stdout);
    return 0;
}

int fflush(FILE *f) { (void)f; return 0; }

/*  vsnprintf  */
int vsnprintf(char *buf, size_t sz, const char *fmt, va_list ap)
{
    size_t pos = 0;
#define PUTC(c) do { if (pos + 1 < sz) buf[pos++] = (c); } while(0)

    while (*fmt) {
        if (*fmt != '%') { PUTC(*fmt++); continue; }
        fmt++;
        /* Flags */
        int zero_pad = 0, left_align = 0;
        while (*fmt == '0') { zero_pad = 1; fmt++; }
        while (*fmt == '-') { left_align = 1; fmt++; }
        /* Width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width*10 + (*fmt++ - '0');
        /* Length modifier */
        int is_long = 0, is_ll = 0;
        if (*fmt == 'l') { is_long = 1; fmt++;
            if (*fmt == 'l') { is_ll = 1; fmt++; } }

        char tmp[32];
        const char *s;
        int len;

        switch (*fmt++) {
        case 'd': case 'i': {
            long long v = is_ll ? va_arg(ap, long long) :
                          is_long ? va_arg(ap, long) : va_arg(ap, int);
            int neg = v < 0; if (neg) v = -v;
            int i = 0;
            if (v == 0) tmp[i++] = '0';
            while (v) { tmp[i++] = '0' + (int)(v % 10); v /= 10; }
            if (neg) tmp[i++] = '-';
            len = i;
            if (!left_align) for (int p = len; p < width; p++) PUTC(zero_pad ? '0' : ' ');
            for (int j = len-1; j >= 0; j--) PUTC(tmp[j]);
            if (left_align)  for (int p = len; p < width; p++) PUTC(' ');
            break;
        }
        case 'u': {
            unsigned long long v = is_ll ? va_arg(ap, unsigned long long) :
                                   is_long ? va_arg(ap, unsigned long) :
                                             va_arg(ap, unsigned int);
            int i = 0;
            if (v == 0) tmp[i++] = '0';
            while (v) { tmp[i++] = '0' + (int)(v % 10); v /= 10; }
            len = i;
            if (!left_align) for (int p = len; p < width; p++) PUTC(zero_pad ? '0' : ' ');
            for (int j = len-1; j >= 0; j--) PUTC(tmp[j]);
            if (left_align)  for (int p = len; p < width; p++) PUTC(' ');
            break;
        }
        case 'x': case 'X': {
            unsigned long long v = is_ll ? va_arg(ap, unsigned long long) :
                                   is_long ? va_arg(ap, unsigned long) :
                                             va_arg(ap, unsigned int);
            const char *hex = (*(fmt-1) == 'X') ? "0123456789ABCDEF"
                                                 : "0123456789abcdef";
            int i = 0;
            if (v == 0) tmp[i++] = '0';
            while (v) { tmp[i++] = hex[v & 0xf]; v >>= 4; }
            len = i;
            if (!left_align) for (int p = len; p < width; p++) PUTC(zero_pad ? '0' : ' ');
            for (int j = len-1; j >= 0; j--) PUTC(tmp[j]);
            if (left_align)  for (int p = len; p < width; p++) PUTC(' ');
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            PUTC('0'); PUTC('x');
            const char *hex = "0123456789abcdef";
            int i = 0;
            if (v == 0) tmp[i++] = '0';
            while (v) { tmp[i++] = hex[v & 0xf]; v >>= 4; }
            for (int j = i-1; j >= 0; j--) PUTC(tmp[j]);
            break;
        }
        case 's': {
            s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            len = (int)strlen(s);
            if (!left_align) for (int p = len; p < width; p++) PUTC(' ');
            while (*s) PUTC(*s++);
            if (left_align)  for (int p = len; p < width; p++) PUTC(' ');
            break;
        }
        case 'c':
            PUTC(va_arg(ap, int));
            break;
        case '%':
            PUTC('%');
            break;
        default:
            PUTC('?');
            break;
        }
    }
    if (pos < sz) buf[pos] = 0;
    return (int)pos;
#undef PUTC
}

int vprintf(const char *fmt, va_list ap)
{
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    write(1, buf, (size_t)n);
    return n;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    write(f->fd, buf, (size_t)n);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/*  File operations  */
FILE *fopen(const char *path, const char *mode)
{
    int flags;
    if (mode[0] == 'r')      flags = O_RDONLY;
    else if (mode[0] == 'w') flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode[0] == 'a') flags = O_WRONLY | O_CREAT | O_APPEND;
    else                     flags = O_RDONLY;

    /* "r+"/"w+"/"a+" all mean read+write */
    if (mode[1] == '+') {
        flags &= ~(O_WRONLY);
        flags |= O_RDWR;
    }

    int fd = open(path, (uint32_t)flags);
    if (fd < 0) return (FILE *)0;
    FILE *f = malloc(sizeof(FILE));
    if (!f) { close(fd); return (FILE *)0; }
    f->fd = fd; f->error = 0; f->eof = 0; f->pushback = -1;
    return f;
}

int fclose(FILE *f)
{
    if (!f) return -1;
    int r = close(f->fd);
    free(f);
    return r;
}

/*
 * freopen — closes the old fd and opens a new file into the SAME
 * FILE* (matching the standard's contract). Exploidus doesn't
 * distinguish text/binary mode (no CRLF translation ever happens),
 * so this is mainly useful for Lua's precompiled-bytecode loading
 * path (lauxlib.c reopens a script file in "rb" after detecting a
 * binary signature) — functionally a no-op mode change here, but
 * still needs to actually re-open the file correctly.
 */
FILE *freopen(const char *path, const char *mode, FILE *f)
{
    if (!f) return (FILE *)0;
    close(f->fd); /* ignore close() errors, matching typical freopen behavior */

    int flags = 0;
    if (mode[0] == 'r') flags = 0;
    else if (mode[0] == 'w') flags = 1;
    else if (mode[0] == 'a') flags = 2;

    int fd = open(path, flags);
    if (fd < 0) { f->error = 1; return (FILE *)0; }

    f->fd = fd; f->error = 0; f->eof = 0; f->pushback = -1;
    return f;
}

/*
 * tmpfile / tmpnam — no real /tmp convention or unlink-on-close in
 * Exploidus's VFS yet, so these create a real file with a generated
 * unique name; unlike POSIX tmpfile() it is NOT automatically deleted
 * when closed (honest limitation — good enough for what Lua's
 * io.tmpfile()/os.tmpname() are typically used for in scripts).
 */
static int g_tmp_counter = 0;

char *tmpnam(char *s)
{
    static char buf[L_tmpnam];
    char *p = s ? s : buf;
    snprintf(p, L_tmpnam, "/tmp_%d.tmp", g_tmp_counter++);
    return p;
}

FILE *tmpfile(void)
{
    char name[L_tmpnam];
    tmpnam(name);
    return fopen(name, "w");
}

/*
 * system — Exploidus has no shell-command-execution path wired up
 * for a non-interactive caller yet (spawn()/spawn_args() exist but
 * launching + waiting on exploish with a one-off command line isn't
 * implemented). Honest stub: report "not supported" rather than
 * silently pretending success.
 */
int system(const char *cmd)
{
    if (!cmd) return 0; /* "is a command processor available?" -> no */
    return -1;
}

int fgetc(FILE *f)
{
    if (f->pushback != -1) {
        int c = f->pushback;
        f->pushback = -1;
        return c;
    }
    char c;
    int64_t r = read(f->fd, &c, 1);
    if (r <= 0) { f->eof = 1; return EOF; }
    return (unsigned char)c;
}

int ungetc(int c, FILE *f)
{
    if (c == EOF) return EOF;
    if (f->pushback != -1) return EOF; /* only one char of pushback supported */
    f->pushback = (unsigned char)c;
    f->eof = 0;
    return (unsigned char)c;
}

void clearerr(FILE *f)
{
    f->error = 0;
    f->eof = 0;
}

/*
 * setvbuf — Exploidus's stdio does unbuffered fd-level I/O for every
 * call already (see fread/fwrite/fgetc above), so there's no actual
 * buffering mode to switch: accept any valid mode and succeed,
 * matching the *behavior* callers asked for (or better) rather than
 * genuinely implementing buffering.
 */
int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    (void)f; (void)buf; (void)size;
    if (mode != _IONBF && mode != _IOFBF && mode != _IOLBF) return -1;
    return 0;
}

int remove(const char *path)
{
    return unlink(path);
}

/*
 * rename — no atomic rename syscall exists yet, so this is a
 * best-effort copy-then-delete fallback: correct for regular files,
 * just not atomic (a crash mid-rename could leave both copies, or
 * neither, unlike a real rename()).
 */
int rename(const char *from, const char *to)
{
    FILE *src = fopen(from, "r");
    if (!src) return -1;

    FILE *dst = fopen(to, "w");
    if (!dst) { fclose(src); return -1; }

    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            fclose(src); fclose(dst);
            return -1;
        }
    }

    fclose(src);
    fclose(dst);
    unlink(from);
    return 0;
}

size_t fread(void *buf, size_t sz, size_t n, FILE *f)
{
    size_t total = sz * n;
    if (!total) return 0;
    int64_t r = read(f->fd, buf, total);
    if (r < 0) { f->error = 1; return 0; }
    if (r == 0) { f->eof = 1; return 0; }
    return (size_t)r / sz;
}

size_t fwrite(const void *buf, size_t sz, size_t n, FILE *f)
{
    size_t total = sz * n;
    if (!total) return 0;
    int64_t r = write(f->fd, buf, total);
    if (r < 0) { f->error = 1; return 0; }
    return (size_t)r / sz;
}

int fseek(FILE *f, long offset, int whence)
{
    return (int)lseek(f->fd, (int64_t)offset, whence);
}

long ftell(FILE *f)
{
    return (long)lseek(f->fd, 0, SEEK_CUR);
}

int feof(FILE *f)   { return f->eof; }
int ferror(FILE *f) { return f->error; }

char *fgets(char *buf, int n, FILE *f)
{
    int i = 0;
    while (i < n - 1) {
        char c;
        int64_t r = read(f->fd, &c, 1);
        if (r <= 0) { if (i == 0) return (char *)0; break; }
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = 0;
    return buf;
}

int getchar(void)
{
    char c;
    int64_t r = read(0, &c, 1);
    return r <= 0 ? EOF : (unsigned char)c;
}