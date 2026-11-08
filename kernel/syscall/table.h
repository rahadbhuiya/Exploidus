#pragma once
#include <stdint.h>
#include "../cap/capability.h"

/* Exploidus syscall numbers */
#define SYS_EXIT          0
#define SYS_FORK          1
#define SYS_EXEC          2
#define SYS_WAITPID       3
#define SYS_OPEN          4
#define SYS_CLOSE         5
#define SYS_READ          6
#define SYS_WRITE         7
#define SYS_MMAP          8
#define SYS_MUNMAP        9
#define SYS_CAP_CREATE   10
#define SYS_CAP_DELEGATE 11
#define SYS_CAP_REVOKE   12
#define SYS_SET_INTENT   13
#define SYS_AUDIT_READ   14
#define SYS_YIELD        15
#define SYS_GETPID       16
#define SYS_SLEEP        17
#define SYS_SOCKET       18
#define SYS_BIND         19
#define SYS_CONNECT      20
#define SYS_LISTEN       21
#define SYS_ACCEPT       22
#define SYS_NET_SEND     23
#define SYS_NET_RECV     24
#define SYS_NET_CLOSE    25
#define SYS_GETIFADDR    26
#define SYS_AUDIT_DUMP   27
#define SYS_GETPROCS     28
#define SYS_FB_PIXEL     29
#define SYS_FB_RECT      30
#define SYS_FB_CLEAR     31
#define SYS_POWEROFF     32
#define SYS_REBOOT       33
#define SYS_FB_INFO      34
#define SYS_READDIR      35
#define SYS_CREATE       36
#define SYS_MOUSE_POS    37
#define SYS_FB_STR       38
#define SYS_SPAWN        39
#define SYS_UPTIME       40
#define SYS_PING         41
#define SYS_FB_CIRCLE    42
#define SYS_FB_RRECT     43
#define SYS_FB_FLIP      44
#define SYS_FB_FLIP_RECT 84   /* partial back->front flip, one rect only */
#define SYS_FB_BLEND_RECT 85  /* alpha-blend a color over a rect (real translucency) */
#define SYS_CHDIR        45
#define SYS_GETCWD         46
#define SYS_CNSL_UNBLOCK   47
#define SYS_CNSL_BLOCK_TTL 48
#define SYS_CNSL_LIST      49
#define SYS_LSEEK          50
#define SYS_STAT           51
#define SYS_FSTAT          52
#define SYS_DUP            53
#define SYS_DUP2           54
#define SYS_HTTP_GET       55   /* download URL to buffer */
#define SYS_FILE_WRITE     56   /* create+write file atomically */
#define SYS_BLAKE3         57   /* hash a userspace buffer */
#define SYS_UNLINK         58   /* remove a file */
#define SYS_HTTP_DOWNLOAD  59   /* stream URL directly to file */
#define SYS_EXECV          60   /* execve with argv array */

/*  GUI: IPC & Shared Memory  */
#define SYS_IPC_SEND       61   /* send message to a process            */
#define SYS_IPC_RECV       62   /* receive message (blocking)           */
#define SYS_IPC_RECV_NB    63   /* receive message (non-blocking)       */
#define SYS_SHM_CREATE     64   /* create shared memory region          */
#define SYS_SHM_MAP        65   /* map shared region into address space */
#define SYS_SHM_UNMAP      66   /* unmap shared region                  */
#define SYS_SHM_DESTROY    67   /* free shared region                   */
#define SYS_FB_CONSOLE_SET 68   /* 0=disable (GUI mode) 1=enable        */
#define SYS_KBD_READ_NB    69   /* non-blocking keyboard read (1 char)  */
#define SYS_KBD_OWNER_SET  70   /* set exclusive keyboard owner PID     */
#define SYS_FB_BLIT        71   /* blit whole ARGB buffer in one call   */
#define SYS_SET_TLS        72   /* set this process's FS_BASE (TLS pointer) */
#define SYS_FUTEX_WAIT     73   /* block if *addr == expected */
#define SYS_FUTEX_WAKE     74   /* wake up to N waiters on addr */
#define SYS_RTC_READ       75   /* read CMOS real-time-clock date/time */
#define SYS_TTY_SET_RAW    76   /* opt this pid out of kernel cooked-mode line editing */
#define SYS_SIGACTION      77   /* register a signal handler with the kernel */
#define SYS_CHMOD          78   /* change a file's permission bits */
#define SYS_RMDIR          79   /* remove an empty directory */
#define SYS_UPTIME_TICKS   80   /* raw 100Hz tick count, no seconds truncation */
#define SYS_SIGRETURN      81   /* resume the code a signal handler interrupted */
#define SYS_KILL           82   /* send a signal to another (or the same) process */
#define SYS_MOUNT          86   /* mount a registered block device (e.g. "usb0")
                                  * as an ExFS volume at a VFS path */
#define SYS_MKFS           87   /* format a registered block device with a
                                  * fresh ExFS volume */
#define SYS_SETUID         88   /* voluntarily drop from UID_ROOT to a
                                  * non-root uid (one-way -- Unix
                                  * privilege-drop semantics, not a real
                                  * setuid()) */
#define SYS_GETUID         89   /* current process's uid */
#define SYS_RENAME         90   /* rename/move a file or empty-or-not
                                  * directory: rename(old_path, new_path).
                                  * See vfs_rename()/exfs_op_rename(). */
#define SYS_DEBUG_EXFS_CRASH 91 /*
 * inside the next ExFS journal commit,
 * to test crash recovery without
 * relying on timing a real one by
 * hand. See exfs_debug_arm_crash() in
 */
#define SYS_SYMLINK        92   /* symlink(target, linkpath) */
#define SYS_READLINK       93   /* readlink(path, buf, bufsize) ->
                                  * bytes written, or -1. NOT
                                  * null-terminated (matches real
                                  * readlink(2)); caller sizes buf and
                                  * checks the return length. */
#define SYS_SETGID         94   /* voluntarily set gid -- see
                                  * sys_setgid()'s comment for the
                                  * exact rule (mirrors SYS_SETUID:
                                  * only while still at UID_ROOT, so
                                  * the correct drop order is
                                  * setgid() then setuid()). */
#define SYS_GETGID         95   /* current process's gid */
#define SYS_CHGRP          96   /* chgrp(path, gid) -- change a
                                  * file's group_gid. Same
                                  * authorization model as
                                  * SYS_CHMOD: no owner/root check
                                  * (pre-existing chmod limitation,
                                  * not something added here). */
#define SYS_DEBUG_EXFS_CORRUPT 97 /*
 * file's first data block,
 * bypassing the normal write path
 * (so block_hash is left stale) --
 * for testing the integrity-hash
 */
#define SYS_EXECVE         98   /*
 * the new image from *path* (unlike
 * SYS_EXEC, which takes an
 * already-in-memory ELF buffer) and
 * replaces the CALLING process's own
 */
#define SYS_MEMINFO        99   /*
 * at rdi with real physical-memory +
 * kernel-heap stats -- backs the
 * shell's `free` command, which used
 * to print a hardcoded placeholder.
 */
#define SYS_GETRANDOM      100  /*
 * data -- RDRAND-backed, same safe
 * generator kernel/cap/capability.c
 * already uses for capability tokens
 * (this syscall duplicates that
 */

#define SYS_PIPE           101
#define SYS_COUNT          102


/*
 * syscall_frame_t
 * Field order MUST exactly match the push sequence in entry.asm.
 * entry.asm pushes (last push = lowest address = offset 0):
 * push user_rsp  -> offset 72
 */
typedef struct __attribute__((packed)) {
    uint64_t r9;       /* offset  0 */
    uint64_t r8;       /* offset  8 */
    uint64_t r10;      /* offset 16 */
    uint64_t rdx;      /* offset 24 */
    uint64_t rsi;      /* offset 32 */
    uint64_t rdi;      /* offset 40 */
    uint64_t rax;      /* offset 48 — syscall number on entry, return value on exit */
    uint64_t r11;      /* offset 56 — user RFLAGS */
    uint64_t rcx;      /* offset 64 — user RIP    */
    uint64_t user_rsp; /* offset 72 — user RSP    */
} syscall_frame_t;


/* Userspace address check */
#define USER_ADDR_MAX  0x00007FFFFFFFFFFFULL

static inline bool uptr_ok(uint64_t addr, uint64_t len)
{
    if (!addr) return false;
    if (addr > USER_ADDR_MAX) return false;
    if (!len)  return true;
    if (len  > USER_ADDR_MAX) return false;
    if (len - 1 > USER_ADDR_MAX - addr) return false;
    return true;
}

typedef int64_t (*syscall_fn_t)(syscall_frame_t *);

/* Syscall Handler Prototypes */
int64_t sys_accept(syscall_frame_t *f);
int64_t sys_audit_dump(syscall_frame_t *f);
int64_t sys_audit_read_h(syscall_frame_t *f);
int64_t sys_bind(syscall_frame_t *f);
int64_t sys_blake3(syscall_frame_t *f);
int64_t sys_cap_create_h(syscall_frame_t *f);
int64_t sys_cap_delegate_h(syscall_frame_t *f);
int64_t sys_cap_revoke_h(syscall_frame_t *f);
int64_t sys_chdir(syscall_frame_t *f);
int64_t sys_chgrp(syscall_frame_t *f);
int64_t sys_chmod(syscall_frame_t *f);
int64_t sys_close(syscall_frame_t *f);
int64_t sys_cnsl_block_ttl(syscall_frame_t *f);
int64_t sys_cnsl_list(syscall_frame_t *f);
int64_t sys_cnsl_unblock(syscall_frame_t *f);
int64_t sys_connect(syscall_frame_t *f);
int64_t sys_create(syscall_frame_t *f);
int64_t sys_debug_exfs_corrupt(syscall_frame_t *f);
int64_t sys_debug_exfs_crash(syscall_frame_t *f);
int64_t sys_dup(syscall_frame_t *f);
int64_t sys_dup2(syscall_frame_t *f);
int64_t sys_pipe(syscall_frame_t *f);
int64_t sys_exec(syscall_frame_t *f);
int64_t sys_execv(syscall_frame_t *f);
int64_t sys_execve(syscall_frame_t *f);
int64_t sys_exit(syscall_frame_t *f);
int64_t sys_fb_blend_rect(syscall_frame_t *f);
int64_t sys_fb_blit(syscall_frame_t *f);
int64_t sys_fb_circle(syscall_frame_t *f);
int64_t sys_fb_clear(syscall_frame_t *f);
int64_t sys_fb_console_set(syscall_frame_t *f);
int64_t sys_fb_flip(syscall_frame_t *f);
int64_t sys_fb_flip_rect(syscall_frame_t *f);
int64_t sys_fb_info(syscall_frame_t *f);
int64_t sys_fb_pixel(syscall_frame_t *f);
int64_t sys_fb_rect(syscall_frame_t *f);
int64_t sys_fb_rrect(syscall_frame_t *f);
int64_t sys_fb_str(syscall_frame_t *f);
int64_t sys_file_write(syscall_frame_t *f);
int64_t sys_fork(syscall_frame_t *f);
int64_t sys_fstat(syscall_frame_t *f);
int64_t sys_futex_wait(syscall_frame_t *f);
int64_t sys_futex_wake(syscall_frame_t *f);
int64_t sys_getcwd(syscall_frame_t *f);
int64_t sys_getgid(syscall_frame_t *f);
int64_t sys_getifaddr(syscall_frame_t *f);
int64_t sys_getpid(syscall_frame_t *f);
int64_t sys_getprocs(syscall_frame_t *f);
int64_t sys_getrandom(syscall_frame_t *f);
int64_t sys_getuid(syscall_frame_t *f);
int64_t sys_http_download(syscall_frame_t *f);
int64_t sys_http_get(syscall_frame_t *f);
int64_t sys_ipc_recv(syscall_frame_t *f);
int64_t sys_ipc_recv_nb(syscall_frame_t *f);
int64_t sys_ipc_send(syscall_frame_t *f);
int64_t sys_kbd_owner_set(syscall_frame_t *f);
int64_t sys_kbd_read_nb(syscall_frame_t *f);
int64_t sys_kill(syscall_frame_t *f);
int64_t sys_listen(syscall_frame_t *f);
int64_t sys_lseek(syscall_frame_t *f);
int64_t sys_meminfo(syscall_frame_t *f);
int64_t sys_mkfs(syscall_frame_t *f);
int64_t sys_mmap(syscall_frame_t *f);
int64_t sys_mount(syscall_frame_t *f);
int64_t sys_mouse_pos(syscall_frame_t *f);
int64_t sys_munmap(syscall_frame_t *f);
int64_t sys_net_close(syscall_frame_t *f);
int64_t sys_net_recv(syscall_frame_t *f);
int64_t sys_net_send(syscall_frame_t *f);
int64_t sys_open(syscall_frame_t *f);
int64_t sys_ping(syscall_frame_t *f);
int64_t sys_poweroff(syscall_frame_t *f);
int64_t sys_read(syscall_frame_t *f);
int64_t sys_readdir(syscall_frame_t *f);
int64_t sys_readlink(syscall_frame_t *f);
int64_t sys_reboot(syscall_frame_t *f);
int64_t sys_rename(syscall_frame_t *f);
int64_t sys_rmdir(syscall_frame_t *f);
int64_t sys_rtc_read(syscall_frame_t *f);
int64_t sys_set_intent(syscall_frame_t *f);
int64_t sys_set_tls(syscall_frame_t *f);
int64_t sys_setgid(syscall_frame_t *f);
int64_t sys_setuid(syscall_frame_t *f);
int64_t sys_shm_create(syscall_frame_t *f);
int64_t sys_shm_destroy(syscall_frame_t *f);
int64_t sys_shm_map(syscall_frame_t *f);
int64_t sys_shm_unmap(syscall_frame_t *f);
int64_t sys_sigaction(syscall_frame_t *f);
int64_t sys_sigreturn(syscall_frame_t *f);
int64_t sys_sleep(syscall_frame_t *f);
int64_t sys_socket(syscall_frame_t *f);
int64_t sys_spawn(syscall_frame_t *f);
int64_t sys_stat(syscall_frame_t *f);
int64_t sys_symlink(syscall_frame_t *f);
int64_t sys_tty_set_raw(syscall_frame_t *f);
int64_t sys_unlink(syscall_frame_t *f);
int64_t sys_uptime(syscall_frame_t *f);
int64_t sys_uptime_ticks(syscall_frame_t *f);
int64_t sys_waitpid(syscall_frame_t *f);
int64_t sys_write(syscall_frame_t *f);
int64_t sys_yield(syscall_frame_t *f);

void syscall_init(void);
void syscall_dispatch(syscall_frame_t *frame);
