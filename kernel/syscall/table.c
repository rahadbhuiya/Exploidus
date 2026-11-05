#include <stdint.h>
#include <stdbool.h>
#include "table.h"
#include "../cap/broker.h"
#include "../cap/capability.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"
#include "../audit/audit.h"
#include "../drivers/serial.h"
#include "../drivers/vga.h"
#include "../drivers/fb.h"
#include "../drivers/font.h"
#include "../drivers/mouse.h"
#include "../drivers/fb_console.h"
#include "../fs/vfs/vfs.h"
#include <string.h>

#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_FMASK  0xC0000084
#define EFER_SCE   (1 << 0)
#define MSR_EFER   0xC0000080

extern void syscall_entry_stub(void);

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}


void syscall_init(void)
{
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | EFER_SCE);
    wrmsr(MSR_STAR, ((uint64_t)0x0010 << 48) | ((uint64_t)0x0008 << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry_stub);
    wrmsr(MSR_FMASK, 0x200);
    serial_print("[SYS] SYSCALL MSR configured\n");
}


/*  Console I/O  */

extern void fb_console_putc(char c);
static int64_t kernel_write(int fd, const void *buf, uint64_t len)
{
    if (fd != 1 && fd != 2) return -1;
    if (!len) return 0;
    if (!buf) return -1;
    const char *p = (const char *)buf;
    for (uint64_t i = 0; i < len; i++) {
        serial_putc(p[i]);
        fb_console_putc(p[i]);
    }
    return (int64_t)len;
}

extern bool keyboard_read(char *out);

/*
 * g_kbd_owner: when non-zero, keyboard_read in kernel_read (fd=0) is
 * suppressed so only the compositor can read keys via SYS_KBD_READ_NB.
 * Set to compositor PID by SYS_KBD_OWNER_SET.  Set to 0 to restore
 * normal blocking reads (text mode).
 */
static uint32_t g_kbd_owner = 0;

/*
 * g_console_raw_pid: pid (if any) that has opted OUT of the cooked-
 * mode line editing below via SYS_TTY_SET_RAW — for a process (like
 * exploish) that already does its own full echo/backspace/history
 * line editing in userspace. Everyone else gets standard terminal
 */
static uint32_t g_console_raw_pid = 0;

#define COOKED_BUF_SIZE 256
static char g_cooked_buf[COOKED_BUF_SIZE];
static int  g_cooked_len       = 0;  /* chars in the line being edited (not yet Enter'd) */
static int  g_cooked_ready_len = 0;  /* length of a completed line waiting to be read, 0 = none */
static int  g_cooked_read_pos  = 0;  /* how much of the ready line has been consumed so far */

static void cooked_echo_putc(char c)
{
    if (fb_console_enabled()) fb_console_putc(c);
    else vga_putc(c);
}

static int64_t kernel_read(int fd, void *buf, uint64_t len)
{
    if (fd != 0) return -1;
    if (!len) return 0;
    if (!buf) return -1;

    /* In GUI mode the compositor owns the keyboard — block console reads */
    if (g_kbd_owner != 0) {
        /* Return 0 bytes so callers don't spin; they will yield/sleep */
        __asm__ volatile ("sti; hlt; cli" ::: "memory");
        return 0;
    }

    bool raw = (g_current_proc && g_current_proc->pid == g_console_raw_pid);

    if (raw) {
        /* Old behavior, unchanged: hand back raw bytes, no echo/editing.
         * This is what exploish opts into so its own line editor keeps
         * working exactly as it did before this feature existed. */
        char    *p = (char *)buf;
        uint64_t n = 0;
        while (n == 0) {
            while (n < len) {
                char c;
                if (!keyboard_read(&c)) break;
                p[n++] = c;
            }
            if (n == 0) {
                /*
 * BUG FIX: this used to be a bare "sti; hlt; cli" --
 * halt until the next interrupt, then loop back and
 * check the keyboard again, all without ever calling
 * sched_yield(). That means the calling process (the
 */
                sched_yield();
            }
        }
        return (int64_t)n;
    }

    /* Cooked mode: echo + backspace-editable line buffering. */
    char *p = (char *)buf;
    uint64_t n = 0;

    while (n == 0) {
        /* Drain a completed line first, if one's waiting. */
        if (g_cooked_ready_len > 0) {
            while (n < len && g_cooked_read_pos < g_cooked_ready_len) {
                p[n++] = g_cooked_buf[g_cooked_read_pos++];
            }
            if (g_cooked_read_pos >= g_cooked_ready_len) {
                g_cooked_ready_len = 0;
                g_cooked_read_pos  = 0;
            }
            if (n > 0) break;
        }

        /* No completed line yet — pull raw keys and edit the
         * in-progress line until Enter completes one. */
        bool got_key = false;
        char c;
        while (keyboard_read(&c)) {
            got_key = true;
            if (c == '\b' || c == 0x7F) {
                if (g_cooked_len > 0) {
                    g_cooked_len--;
                    cooked_echo_putc('\b');
                    cooked_echo_putc(' ');
                    cooked_echo_putc('\b');
                }
                continue;
            }
            if (c == '\n' || c == '\r') {
                cooked_echo_putc('\n');
                if (g_cooked_len < COOKED_BUF_SIZE - 1) {
                    g_cooked_buf[g_cooked_len++] = '\n';
                }
                g_cooked_ready_len = g_cooked_len;
                g_cooked_read_pos  = 0;
                g_cooked_len       = 0;
                break; /* line complete — go drain it above */
            }
            if (g_cooked_len < COOKED_BUF_SIZE - 1) {
                g_cooked_buf[g_cooked_len++] = c;
                cooked_echo_putc(c);
            }
        }

        if (!got_key) {
            /* Same fix as the raw-mode branch above -- see its comment. */
            sched_yield();
        }
    }

    return (int64_t)n;
}




int64_t sys_read(syscall_frame_t *f)
{
    int fd = (int)(int64_t)f->rdi;
    if (fd == 0) {
        if (!uptr_ok(f->rsi, f->rdx)) return -1;
        return kernel_read(fd, (void *)(uintptr_t)f->rsi, f->rdx);
    }
    if (!uptr_ok(f->rsi, f->rdx)) return -1;
    return vfs_read(fd, (void *)(uintptr_t)f->rsi, f->rdx);
}


int64_t sys_write(syscall_frame_t *f)
{
    int fd = (int)(int64_t)f->rdi;
    if (fd == 1 || fd == 2) {
        if (!uptr_ok(f->rsi, f->rdx)) return -1;
        return kernel_write(fd, (const void *)(uintptr_t)f->rsi, f->rdx);
    }
    if (!uptr_ok(f->rsi, f->rdx)) return -1;
    return vfs_write(fd, (const void *)(uintptr_t)f->rsi, f->rdx);
}



/*
 * sys_tty_set_raw(raw) — opts the calling process out of (raw=1) or
 * back into (raw=0) kernel-side cooked-mode line editing for stdin.
 * exploish calls this once at startup since it already implements its
 * own full line editor; every other program gets normal echo +
 * backspace handling by default without needing to know this exists.
 */
int64_t sys_tty_set_raw(syscall_frame_t *f)
{
    int raw = (int)f->rdi;
    if (!g_current_proc) return -1;
    g_console_raw_pid = raw ? g_current_proc->pid : 0;
    return 0;
}



/*
 * SYS_KBD_READ_NB(69) — non-blocking keyboard poll.
 *   rdi = pointer to a single char output buffer
 * Returns 1 if a key was read, 0 if no key available, -1 on bad pointer.
 * Used by the compositor to poll keyboard without blocking its event loop.
 */
int64_t sys_kbd_read_nb(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;

    /* Only the registered keyboard owner can poll keys */
    if (g_kbd_owner != 0 && g_current_proc &&
        g_current_proc->pid != g_kbd_owner)
        return 0;  /* not the owner — return "no key" */

    char *out = (char *)(uintptr_t)f->rdi;
    char c;
    if (keyboard_read(&c)) {
        *out = c;
        return 1;
    }
    return 0;
}



/*
 * SYS_KBD_OWNER_SET(70)
 *   rdi = PID to give exclusive keyboard ownership (0 = release, text mode)
 * Only the current process or PID 0 can call this.
 * Returns previous owner PID.
 */
int64_t sys_kbd_owner_set(syscall_frame_t *f)
{
    uint32_t prev = g_kbd_owner;
    g_kbd_owner   = (uint32_t)f->rdi;
    serial_print("[KBD ] owner set to PID ");
    serial_printhex((uint64_t)g_kbd_owner);
    serial_print("\n");
    return (int64_t)prev;
}



/*
 * SYS_FB_CONSOLE_SET(68)
 *   rdi = 0 → disable fb_console (GUI mode, compositor owns screen)
 *   rdi = 1 → enable  fb_console (text mode, default)
 * Returns previous state (0 or 1).
 */
int64_t sys_fb_console_set(syscall_frame_t *f)
{
    int prev = fb_console_enabled();
    if (f->rdi == 0)
        fb_console_disable();
    else
        fb_console_enable();
    return (int64_t)prev;
}



int64_t sys_fb_pixel(syscall_frame_t *f)
{ fb_put_pixel((uint32_t)f->rdi, (uint32_t)f->rsi, (uint32_t)f->rdx); return 0; }


int64_t sys_fb_rect(syscall_frame_t *f)
{ fb_fill_rect((uint32_t)f->rdi,(uint32_t)f->rsi,(uint32_t)f->rdx,(uint32_t)f->r10,(uint32_t)f->r8); return 0; }


int64_t sys_fb_clear(syscall_frame_t *f)
{ fb_clear((uint32_t)f->rdi); return 0; }


int64_t sys_fb_info(syscall_frame_t *f)
{
    uint32_t *out = (uint32_t *)(uintptr_t)f->rdi;
    if (!uptr_ok(f->rdi, 16)) return -1;
    out[0] = g_fb.width; out[1] = g_fb.height; out[2] = g_fb.active ? 1 : 0; out[3] = g_fb.bpp;
    return 0;
}


int64_t sys_mouse_pos(syscall_frame_t *f)
{
    int32_t *out = (int32_t *)(uintptr_t)f->rdi;
    if (!uptr_ok(f->rdi, 12)) return -1;
    out[0] = g_mouse.x; out[1] = g_mouse.y;
    out[2] = (g_mouse.left ? 1 : 0) | (g_mouse.right ? 2 : 0) | (g_mouse.middle ? 4 : 0);
    return 0;
}


int64_t sys_fb_str(syscall_frame_t *f)
{
    uint32_t x = (uint32_t)f->rdi; uint32_t y = (uint32_t)f->rsi;
    uint64_t ptr = f->rdx; uint32_t fg = (uint32_t)f->r10; uint32_t bg = (uint32_t)f->r8;
    if (!uptr_ok(ptr, 1)) return -1;
    font_draw_str(x, y, (const char *)(uintptr_t)ptr, fg, bg);
    return 0;
}


int64_t sys_fb_circle(syscall_frame_t *f)
{ fb_fill_circle((int)f->rdi,(int)f->rsi,(int)f->rdx,(uint32_t)f->r10); return 0; }


int64_t sys_fb_rrect(syscall_frame_t *f)
{ fb_fill_rounded_rect((int)f->rdi,(int)f->rsi,(int)f->rdx,(int)f->r10,(int)f->r8,(uint32_t)f->r9); return 0; }


int64_t sys_fb_flip(syscall_frame_t *f)
{ (void)f; fb_flip(); return 0; }



/*
 * sys_fb_flip_rect(x, y, w, h) -- same as fb_flip() but copies only
 * one rect from the back buffer to the real framebuffer, instead of
 * the whole screen. See fb_flip_rect() in kernel/drivers/fb.c.
 */
int64_t sys_fb_flip_rect(syscall_frame_t *f)
{
    int32_t  x = (int32_t)f->rdi;
    int32_t  y = (int32_t)f->rsi;
    uint32_t w = (uint32_t)f->rdx;
    uint32_t h = (uint32_t)f->r10;
    fb_flip_rect(x, y, w, h);
    return 0;
}



/*
 * sys_fb_blend_rect(x, y, w, h, color, alpha) -- alpha-blends color
 * over whatever is already drawn in that rect. See fb_blend_rect()
 * in kernel/drivers/fb.c.
 */
int64_t sys_fb_blend_rect(syscall_frame_t *f)
{
    uint32_t x     = (uint32_t)f->rdi;
    uint32_t y     = (uint32_t)f->rsi;
    uint32_t w     = (uint32_t)f->rdx;
    uint32_t h     = (uint32_t)f->r10;
    uint32_t color = (uint32_t)f->r8;
    uint8_t  alpha = (uint8_t)f->r9;
    fb_blend_rect(x, y, w, h, color, alpha);
    return 0;
}



/*
 * sys_fb_blit(dst_x, dst_y, w, h, src_ptr, bg_color)
 * Blits a whole ARGB32 window buffer in one syscall instead of the
 * caller doing one fb_pixel syscall per pixel (which was the main
 * cost behind laggy/flickery window redraws and drags).
 */
int64_t sys_fb_blit(syscall_frame_t *f)
{
    int32_t  dst_x = (int32_t)f->rdi;
    int32_t  dst_y = (int32_t)f->rsi;
    uint32_t w     = (uint32_t)f->rdx;
    uint32_t h     = (uint32_t)f->r10;
    uint64_t src   = f->r8;
    uint32_t bg    = (uint32_t)f->r9;

    if (w == 0 || h == 0) return 0;
    uint64_t len = (uint64_t)w * (uint64_t)h * 4;
    if (!uptr_ok(src, len)) return -1;

    fb_blit(dst_x, dst_y, w, h, (const uint32_t *)(uintptr_t)src, bg);
    return 0;
}


static const syscall_fn_t g_syscall_table[SYS_COUNT] = {
    [SYS_EXIT]         = sys_exit,
    [SYS_FORK]         = sys_fork,
    [SYS_EXEC]         = sys_exec,
    [SYS_WAITPID]      = sys_waitpid,
    [SYS_OPEN]         = sys_open,
    [SYS_CLOSE]        = sys_close,
    [SYS_READ]         = sys_read,
    [SYS_WRITE]        = sys_write,
    [SYS_MMAP]         = sys_mmap,
    [SYS_MUNMAP]       = sys_munmap,
    [SYS_CAP_CREATE]   = sys_cap_create_h,
    [SYS_CAP_DELEGATE] = sys_cap_delegate_h,
    [SYS_CAP_REVOKE]   = sys_cap_revoke_h,
    [SYS_SET_INTENT]   = sys_set_intent,
    [SYS_AUDIT_READ]   = sys_audit_read_h,
    [SYS_AUDIT_DUMP]   = sys_audit_dump,
    [SYS_YIELD]        = sys_yield,
    [SYS_GETPID]       = sys_getpid,
    [SYS_SLEEP]        = sys_sleep,
    [SYS_GETPROCS]     = sys_getprocs,
    [SYS_POWEROFF]     = sys_poweroff,
    [SYS_REBOOT]       = sys_reboot,
    [SYS_SPAWN]        = sys_spawn,
    [SYS_UPTIME]       = sys_uptime,
    [SYS_UPTIME_TICKS] = sys_uptime_ticks,
    [SYS_MOUSE_POS]    = sys_mouse_pos,
    [SYS_FB_STR]       = sys_fb_str,
    [SYS_FB_PIXEL]     = sys_fb_pixel,
    [SYS_FB_RECT]      = sys_fb_rect,
    [SYS_FB_CLEAR]     = sys_fb_clear,
    [SYS_FB_INFO]      = sys_fb_info,
    [SYS_READDIR]      = sys_readdir,
    [SYS_CREATE]       = sys_create,
    [SYS_SOCKET]       = sys_socket,
    [SYS_BIND]         = sys_bind,
    [SYS_CONNECT]      = sys_connect,
    [SYS_LISTEN]       = sys_listen,
    [SYS_ACCEPT]       = sys_accept,
    [SYS_NET_SEND]     = sys_net_send,
    [SYS_NET_RECV]     = sys_net_recv,
    [SYS_NET_CLOSE]    = sys_net_close,
    [SYS_GETIFADDR]    = sys_getifaddr,
    [SYS_PING]         = sys_ping,
    [SYS_FB_CIRCLE]    = sys_fb_circle,
    [SYS_FB_RRECT]     = sys_fb_rrect,
    [SYS_FB_FLIP]      = sys_fb_flip,
    [SYS_CHDIR]        = sys_chdir,
    [SYS_GETCWD]       = sys_getcwd,
    [SYS_CNSL_UNBLOCK]   = sys_cnsl_unblock,
    [SYS_CNSL_BLOCK_TTL] = sys_cnsl_block_ttl,
    [SYS_CNSL_LIST]      = sys_cnsl_list,
    [SYS_LSEEK]        = sys_lseek,
    [SYS_STAT]         = sys_stat,
    [SYS_FSTAT]        = sys_fstat,
    [SYS_DUP]          = sys_dup,
    [SYS_DUP2]         = sys_dup2,
    [SYS_HTTP_GET]     = sys_http_get,
    [SYS_FILE_WRITE]   = sys_file_write,
    [SYS_BLAKE3]       = sys_blake3,
    [SYS_UNLINK]       = sys_unlink,
    [SYS_RENAME]       = sys_rename,
    [SYS_DEBUG_EXFS_CRASH] = sys_debug_exfs_crash,
    [SYS_SYMLINK]      = sys_symlink,
    [SYS_READLINK]     = sys_readlink,
    [SYS_SETGID]       = sys_setgid,
    [SYS_GETGID]       = sys_getgid,
    [SYS_CHGRP]        = sys_chgrp,
    [SYS_DEBUG_EXFS_CORRUPT] = sys_debug_exfs_corrupt,
    [SYS_HTTP_DOWNLOAD]= sys_http_download,
    [SYS_EXECV]        = sys_execv,
    [SYS_EXECVE]       = sys_execve,
    [SYS_MEMINFO]      = sys_meminfo,
    [SYS_GETRANDOM]    = sys_getrandom,
    /* GUI Phase 1 */
    [SYS_IPC_SEND]     = sys_ipc_send,
    [SYS_IPC_RECV]     = sys_ipc_recv,
    [SYS_IPC_RECV_NB]  = sys_ipc_recv_nb,
    [SYS_SHM_CREATE]   = sys_shm_create,
    [SYS_SHM_MAP]      = sys_shm_map,
    [SYS_SHM_UNMAP]    = sys_shm_unmap,
    [SYS_SHM_DESTROY]  = sys_shm_destroy,
    /* GUI Phase 2 */
    [SYS_FB_CONSOLE_SET] = sys_fb_console_set,
    [SYS_KBD_READ_NB]    = sys_kbd_read_nb,
    [SYS_KBD_OWNER_SET]  = sys_kbd_owner_set,
    [SYS_FB_BLIT]        = sys_fb_blit,
    [SYS_SET_TLS]        = sys_set_tls,
    [SYS_FUTEX_WAIT]     = sys_futex_wait,
    [SYS_FUTEX_WAKE]     = sys_futex_wake,
    [SYS_RTC_READ]       = sys_rtc_read,
    [SYS_TTY_SET_RAW]    = sys_tty_set_raw,
    [SYS_SIGACTION]      = sys_sigaction,
    [SYS_CHMOD]          = sys_chmod,
    [SYS_RMDIR]          = sys_rmdir,
    [SYS_SIGRETURN]      = sys_sigreturn,
    [SYS_KILL]           = sys_kill,
    [SYS_MOUNT]          = sys_mount,
    [SYS_MKFS]           = sys_mkfs,
    [SYS_SETUID]         = sys_setuid,
    [SYS_GETUID]         = sys_getuid,
    [SYS_FB_FLIP_RECT]   = sys_fb_flip_rect,
    [SYS_FB_BLEND_RECT]  = sys_fb_blend_rect,
};

void syscall_dispatch(syscall_frame_t *frame)
{
    uint64_t num = frame->rax;
    audit_record(AUDIT_SYSCALL, g_current_proc ? g_current_proc->pid : 0, num, frame->rdi);
    if (num >= SYS_COUNT || !g_syscall_table[num]) {
        serial_print("[SYS] unhandled "); serial_printhex(num); serial_print("\n");
        frame->rax = (uint64_t)-1LL;
        return;
    }
    frame->rax = (uint64_t)g_syscall_table[num](frame);
}