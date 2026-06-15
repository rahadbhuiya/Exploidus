#pragma once
/*
 * Exploidus musl Compatibility Layer
 * userspace/musl/musl_syscall.h
 *
 * Maps Linux x86-64 syscall numbers to Exploidus syscall numbers.
 * musl compiled with -DEXPLOIDUS will call __exploidus_syscall()
 * instead of the raw Linux syscall instruction.
 *
 * Linux syscall numbers (x86-64 ABI):
 *   https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl
 */

#include <stdint.h>
#include "../libc/syscall.h"

/*  Linux syscall numbers we intercept  */
#define LINUX_SYS_read          0
#define LINUX_SYS_write         1
#define LINUX_SYS_open          2
#define LINUX_SYS_close         3
#define LINUX_SYS_mmap          9
#define LINUX_SYS_munmap        11
#define LINUX_SYS_exit          60
#define LINUX_SYS_fork          57
#define LINUX_SYS_execve        59
#define LINUX_SYS_wait4         61
#define LINUX_SYS_getpid        39
#define LINUX_SYS_socket        41
#define LINUX_SYS_bind          49
#define LINUX_SYS_connect       42
#define LINUX_SYS_listen        50
#define LINUX_SYS_accept        43
#define LINUX_SYS_sendto        44
#define LINUX_SYS_recvfrom      45
#define LINUX_SYS_sched_yield   24
#define LINUX_SYS_nanosleep     35
#define LINUX_SYS_getcwd        79
#define LINUX_SYS_chdir         80
#define LINUX_SYS_mkdir         83
#define LINUX_SYS_getdents64    217
#define LINUX_SYS_uname         63
#define LINUX_SYS_getuid        102
#define LINUX_SYS_getgid        104
#define LINUX_SYS_geteuid       107
#define LINUX_SYS_getegid       108
#define LINUX_SYS_exit_group    231
#define LINUX_SYS_arch_prctl    158
#define LINUX_SYS_set_tid_addr  218
#define LINUX_SYS_set_robust_list 273
#define LINUX_SYS_clock_gettime 228
#define LINUX_SYS_futex         202
#define LINUX_SYS_mprotect      10
#define LINUX_SYS_brk           12
#define LINUX_SYS_ioctl         16
#define LINUX_SYS_writev        20
#define LINUX_SYS_access        21
#define LINUX_SYS_dup           32
#define LINUX_SYS_dup2          33
#define LINUX_SYS_fcntl         72
#define LINUX_SYS_fstat         5
#define LINUX_SYS_lseek         8
#define LINUX_SYS_stat          4
#define LINUX_SYS_pread64       17
#define LINUX_SYS_pwrite64      18

/*  uname struct (returned for uname() compat)  */
typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
} exploidus_utsname_t;

/*  Stub return values for unsupported-but-safe syscalls  */
#define ENOSYS  38
#define EPERM    1
#define ENOENT   2

/*  The main dispatcher  */
static inline int64_t __exploidus_syscall(
    uint64_t nr,
    uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;

    switch (nr) {
    /*  I/O  */
    case LINUX_SYS_read:
        return (int64_t)read((int)a0, (void *)(uintptr_t)a1, (uint64_t)a2);
    case LINUX_SYS_write:
        return (int64_t)write((int)a0, (const void *)(uintptr_t)a1, (uint64_t)a2);
    case LINUX_SYS_writev: {
        /* writev: scatter write — flatten into sequential writes */
        typedef struct { uint64_t base; uint64_t len; } iovec_t;
        iovec_t *iov = (iovec_t *)(uintptr_t)a1;
        int64_t total = 0;
        for (uint64_t i = 0; i < a2; i++)
            total += write((int)a0, (const void *)(uintptr_t)iov[i].base, iov[i].len);
        return total;
    }

    /*  Files  */
    case LINUX_SYS_open:
        return (int64_t)open((const char *)(uintptr_t)a0, (int)a1);
    case LINUX_SYS_close:
        return (int64_t)close((int)a0);
    case LINUX_SYS_lseek:
        return (int64_t)lseek((int)a0, (int64_t)a1, (int)a2);
    case LINUX_SYS_pread64:
        /* pread: seek + read + restore */
        { int64_t saved = lseek((int)a0, 0, SEEK_CUR);
          lseek((int)a0, (int64_t)a3, SEEK_SET);
          int64_t n = read((int)a0, (void *)(uintptr_t)a1, a2);
          lseek((int)a0, saved, SEEK_SET);
          return n; }
    case LINUX_SYS_pwrite64:
        { int64_t saved = lseek((int)a0, 0, SEEK_CUR);
          lseek((int)a0, (int64_t)a3, SEEK_SET);
          int64_t n = write((int)a0, (const void *)(uintptr_t)a1, a2);
          lseek((int)a0, saved, SEEK_SET);
          return n; }
    case LINUX_SYS_stat:
        { vfs_stat_t st; int r = stat((const char *)(uintptr_t)a0, &st);
          if (r == 0 && a1) {
              /* fill Linux stat64 layout: st_size at offset 48 */
              uint64_t *linux_st = (uint64_t *)(uintptr_t)a1;
              for(int _i=0;_i<18;_i++) linux_st[_i]=0;
              linux_st[6] = st.size;   /* st_size */
              linux_st[1] = st.inode;  /* st_ino  */
          }
          return r; }
    case LINUX_SYS_fstat:
        { vfs_stat_t st; int r = fstat((int)a0, &st);
          if (r == 0 && a1) {
              uint64_t *linux_st = (uint64_t *)(uintptr_t)a1;
              for(int _i=0;_i<18;_i++) linux_st[_i]=0;
              linux_st[6] = st.size;
              linux_st[1] = st.inode;
          }
          return r; }
    case LINUX_SYS_dup:
        return (int64_t)dup((int)a0);
    case LINUX_SYS_dup2:
        return (int64_t)dup2((int)a0, (int)a1);
    case LINUX_SYS_fcntl:
        return 0;       /* no-op: musl uses this for FD_CLOEXEC etc */
    case LINUX_SYS_ioctl:
        return 0;       /* no-op: musl uses this for terminal detection */
    case LINUX_SYS_getcwd:
        return (int64_t)getcwd((char *)(uintptr_t)a0, (uint64_t)a1);
    case LINUX_SYS_chdir:
        return (int64_t)chdir((const char *)(uintptr_t)a0);
    case LINUX_SYS_mkdir:
        return (int64_t)vfs_create((const char *)(uintptr_t)a0, 1);
    case LINUX_SYS_getdents64:
        return (int64_t)readdir((int)a0, (void *)(uintptr_t)a1, a2);

    /*  Memory  */
    case LINUX_SYS_mmap:
        /* Linux mmap: a0=addr a1=len a2=prot a3=flags a4=fd a5=offset
         * Exploidus anonymous mmap ignores addr/prot/flags/fd/offset */
        return (int64_t)mmap(a1);
    case LINUX_SYS_munmap:
        return (int64_t)munmap((void *)(uintptr_t)a0, a1);
    case LINUX_SYS_mprotect:
        return 0;   /* no-op: Exploidus VMM doesn't support mprotect yet */
    case LINUX_SYS_brk:
        return 0;   /* musl falls back to mmap if brk returns 0 */

    /*  Process  */
    case LINUX_SYS_fork:
        return (int64_t)fork();
    case LINUX_SYS_execve:
        /* a0=path a1=argv a2=envp — Exploidus exec doesn't pass args yet */
        return (int64_t)spawn((const char *)(uintptr_t)a0);
    case LINUX_SYS_exit:
    case LINUX_SYS_exit_group:
        exit((int)a0);
        return 0;
    case LINUX_SYS_wait4:
        return (int64_t)waitpid((int64_t)a0);
    case LINUX_SYS_getpid:
        return (int64_t)getpid();
    case LINUX_SYS_sched_yield:
        return (int64_t)yield();
    case LINUX_SYS_nanosleep: {
        /* timespec: { uint64_t sec, uint64_t nsec } */
        uint64_t *ts = (uint64_t *)(uintptr_t)a0;
        uint64_t ticks = ts[0] * 100 + ts[1] / 10000000; /* 100Hz */
        if (ticks == 0) ticks = 1;
        return (int64_t)sleep_ticks(ticks);
    }

    /*  Network  */
    case LINUX_SYS_socket:
        return (int64_t)net_socket((sock_type_t)a1);
    case LINUX_SYS_bind:
        return (int64_t)net_bind((int)a0, (uint16_t)a2);
    case LINUX_SYS_connect:
        return (int64_t)net_connect((int)a0, (ip4_t)a1, (uint16_t)a2);
    case LINUX_SYS_listen:
        return (int64_t)net_listen((int)a0);
    case LINUX_SYS_accept:
        return (int64_t)net_accept((int)a0);
    case LINUX_SYS_sendto:
        return (int64_t)net_send((int)a0,
            (const void *)(uintptr_t)a1, (uint16_t)a2);
    case LINUX_SYS_recvfrom:
        return (int64_t)net_recv((int)a0,
            (void *)(uintptr_t)a1, (uint16_t)a2);

    /*  Identity — return safe fake values  */
    case LINUX_SYS_getuid:
    case LINUX_SYS_geteuid:
    case LINUX_SYS_getgid:
    case LINUX_SYS_getegid:
        return 0; /* root */

    /*  uname  */
    case LINUX_SYS_uname: {
        exploidus_utsname_t *u = (exploidus_utsname_t *)(uintptr_t)a0;
        if (!u) return -EPERM;
        /* copy strings safely without libc */
        const char *sysname  = "Exploidus";
        const char *nodename = "exploidus";
        const char *release  = "0.1.0";
        const char *version  = "#1 Exploidus Kernel";
        const char *machine  = "x86_64";
        int i;
        for(i=0;sysname[i]&&i<64;i++)  u->sysname[i]=sysname[i];   u->sysname[i]=0;
        for(i=0;nodename[i]&&i<64;i++) u->nodename[i]=nodename[i]; u->nodename[i]=0;
        for(i=0;release[i]&&i<64;i++)  u->release[i]=release[i];   u->release[i]=0;
        for(i=0;version[i]&&i<64;i++)  u->version[i]=version[i];   u->version[i]=0;
        for(i=0;machine[i]&&i<64;i++)  u->machine[i]=machine[i];   u->machine[i]=0;
        return 0;
    }

    /*  musl thread/TLS init stubs — safe no-ops  */
    case LINUX_SYS_arch_prctl:
        return 0; /* musl uses this to set FS base for TLS */
    case LINUX_SYS_set_tid_addr:
        return (int64_t)getpid();
    case LINUX_SYS_set_robust_list:
        return 0;
    case LINUX_SYS_futex:
        return 0; /* single-threaded: futex is always uncontested */
    case LINUX_SYS_clock_gettime: {
        /* CLOCK_REALTIME / CLOCK_MONOTONIC — return uptime as seconds */
        uint64_t *ts = (uint64_t *)(uintptr_t)a1;
        if (ts) { ts[0] = (uint64_t)uptime(); ts[1] = 0; }
        return 0;
    }

    default:
        return -(int64_t)ENOSYS;
    }
}