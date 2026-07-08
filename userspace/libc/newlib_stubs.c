#include "syscall.h"
#include "errno.h"
#include "stdlib.h"
#include <stdint.h>
#include <stddef.h>

/*
 * userspace/libc/newlib_stubs.c
 *
 * This is the "libgloss"-style glue layer: the small set of functions
 * newlib expects the platform to provide (conventionally named with a
 * leading underscore), each implemented on top of Exploidus's own
 * native syscalls. When newlib is actually built for Exploidus, this
 * file compiles straight into that build and satisfies newlib's
 * undefined syscall-stub symbols.
 *
 * The plain (non-underscore) POSIX names are also provided since
 * they're directly useful right now, before newlib exists.
 */


/* isatty */


/*
 * Exploidus doesn't have a device-file abstraction yet — fds 0/1/2
 * are always the console (stdin/stdout/stderr) in this OS's process
 * model, everything else is a regular VFS file. Good enough for what
 * isatty() is actually used for (stdio deciding line- vs full-
 * buffering).
 */
int isatty(int fd)
{
    return (fd == 0 || fd == 1 || fd == 2) ? 1 : 0;
}

int _isatty(int fd) { return isatty(fd); }


/* kill */

/*
 * Honest limitation: Exploidus has no real signal-delivery mechanism
 * yet, and no way to terminate an arbitrary OTHER process (the
 * shell's own `kill <pid>` command is currently a stub too — see
 * cmd_ext_kill in userspace/shell/exploish_cmds.c). What DOES work,
 * and covers the main real-world use of kill() from ported code
 * (raise()/abort() targeting yourself with SIGABRT), is self-signaling.
 */
int kill(int pid, int sig)
{
    (void)sig;
    if (pid == (int)getpid()) {
        exit(128 + (sig > 0 ? sig : 0)); /* conventional shell exit-code style */
    }
    errno = ESRCH; /* no such process we can actually reach */
    return -1;
}

int _kill(int pid, int sig) { return kill(pid, sig); }


/* sbrk */


/*
 * Exploidus's own malloc() already uses mmap() directly and doesn't
 * need sbrk at all — this exists purely so a ported libc (newlib's
 * default malloc, or any other code that assumes classic sbrk
 * semantics) has something that behaves correctly.
 *
 * Real sbrk grows one single contiguous, ever-upward heap region.
 * mmap() here allocates fresh regions on each call and (per its
 * kernel implementation) happens to place them adjacently, but
 * growing DOWNWARD — the opposite direction most mallocs assume. So
 * instead of calling mmap() per-increment, reserve one region ONCE,
 * upfront, and hand out increments from within it via a simple bump
 * pointer. This exactly matches what real sbrk callers expect.
 */

#define SBRK_ARENA_SIZE (4u * 1024u * 1024u) /* 4 MiB — tune as needed */

static uint8_t *g_sbrk_base = NULL;
static uint8_t *g_sbrk_cur  = NULL;
static uint8_t *g_sbrk_end  = NULL;

void *sbrk(intptr_t incr)
{
    if (!g_sbrk_base) {
        void *p = (void *)(uintptr_t)mmap(SBRK_ARENA_SIZE);
        if (!p) { errno = ENOMEM; return (void *)-1; }
        g_sbrk_base = (uint8_t *)p;
        g_sbrk_cur  = g_sbrk_base;
        g_sbrk_end  = g_sbrk_base + SBRK_ARENA_SIZE;
    }

    if (incr < 0) {
        /* Shrinking: only ever back up within what we've already
         * handed out — never below the arena's start. */
        if (g_sbrk_cur + incr < g_sbrk_base) {
            errno = EINVAL;
            return (void *)-1;
        }
        uint8_t *prev = g_sbrk_cur;
        g_sbrk_cur += incr;
        return prev;
    }

    if (g_sbrk_cur + incr > g_sbrk_end) {
        errno = ENOMEM;
        return (void *)-1;
    }
    uint8_t *prev = g_sbrk_cur;
    g_sbrk_cur += incr;
    return prev;
}

void *_sbrk(intptr_t incr) { return sbrk(incr); }


/* Thin renames for the rest of newlib's expected stub names —      */
/* Exploidus's own libc already implements the real logic for all   */
/* of these; newlib just needs the underscore-prefixed symbol names. */

int    _read(int fd, void *buf, size_t len)      { return (int)read(fd, buf, len); }
int    _write(int fd, const void *buf, size_t len) { return (int)write(fd, buf, len); }
int    _close(int fd)                             { return close(fd); }
int    _lseek(int fd, int offset, int whence)     { return (int)lseek(fd, offset, whence); }
int    _getpid(void)                              { return (int)getpid(); }
void   _exit(int status) __attribute__((noreturn));
void   _exit(int status)                          { exit(status); __builtin_unreachable(); }