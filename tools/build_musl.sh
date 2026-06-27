#!/bin/bash
# tools/build_musl.sh
# Builds musl libc patched for Exploidus syscall ABI.
# Run from repo root: bash tools/build_musl.sh
set -e

MUSL_VER="1.2.5"
CROSS="$HOME/opt/cross/bin/x86_64-elf"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT="${REPO}/exploidus-sysroot"
BUILD_DIR="${REPO}/musl-${MUSL_VER}"

#  pre-flight 
echo "========================================"
echo " Exploidus musl build"
echo " musl version : ${MUSL_VER}"
echo " sysroot      : ${SYSROOT}"
echo " cross        : ${CROSS}-gcc"
echo "========================================"

if [ ! -f "${CROSS}-gcc" ]; then
    echo "[ERROR] Cross compiler not found: ${CROSS}-gcc"
    echo "        Run: bash setup.sh"
    exit 1
fi

#  step 1: download 
if [ ! -d "${BUILD_DIR}" ]; then
    echo "[1/5] Downloading musl-${MUSL_VER}..."
    cd "${REPO}"
    wget -q --show-progress \
        "https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz"
    tar -xzf "musl-${MUSL_VER}.tar.gz"
    rm "musl-${MUSL_VER}.tar.gz"
else
    echo "[1/5] musl-${MUSL_VER} already extracted — skipping download."
fi

cd "${BUILD_DIR}"

#  step 2: write userspace/musl/musl_syscall.h 
# Self-contained: no external includes, all types and SYS_* constants inline.
# This avoids stdint.h / capability.h / stdbool.h chain under -nostdinc.
echo "[2/5] Writing ${REPO}/userspace/musl/musl_syscall.h ..."
mkdir -p "${REPO}/userspace/musl"
cat > "${REPO}/userspace/musl/musl_syscall.h" << 'SHIM_EOF'
#pragma once
/*
 * Exploidus musl Compatibility Layer
 * Maps Linux x86-64 syscall numbers → Exploidus syscall numbers.
 * Fully self-contained — no external includes.
 */
#ifndef __EXPLOIDUS_MUSL_SYSCALL_H
#define __EXPLOIDUS_MUSL_SYSCALL_H

/*  private integer types (GCC built-ins — never conflict with musl)  */
typedef __UINT8_TYPE__   __x_u8;
typedef __UINT16_TYPE__  __x_u16;
typedef __UINT32_TYPE__  __x_u32;
typedef __UINT64_TYPE__  __x_u64;
typedef __INT64_TYPE__   __x_i64;
typedef __SIZE_TYPE__    __x_usz;
typedef __UINTPTR_TYPE__ __x_uptr;

/* uint64_t / int64_t aliases for internal use only */
#define _xu64 __x_u64
#define _xi64 __x_i64
#define _xu8  __x_u8
#define _xu16 __x_u16
#define _xu32 __x_u32

/*  Exploidus syscall numbers (copy of kernel/syscall/table.h)  */
#define XSC_EXIT         0
#define XSC_FORK         1
#define XSC_EXEC         2
#define XSC_WAITPID      3
#define XSC_OPEN         4
#define XSC_CLOSE        5
#define XSC_READ         6
#define XSC_WRITE        7
#define XSC_GETPID       8
#define XSC_SLEEP        9
#define XSC_MMAP        10
#define XSC_MUNMAP      11
#define XSC_YIELD       12
#define XSC_SOCKET      13
#define XSC_BIND        14
#define XSC_CONNECT     15
#define XSC_LISTEN      16
#define XSC_ACCEPT      17
#define XSC_NET_SEND    18
#define XSC_NET_RECV    19
#define XSC_STAT        20
#define XSC_FSTAT       21
#define XSC_LSEEK       22
#define XSC_DUP         23
#define XSC_DUP2        24
#define XSC_GETCWD      25
#define XSC_CHDIR       26
#define XSC_CREATE      27
#define XSC_UNLINK      28
#define XSC_READDIR     29
#define XSC_SPAWN       39
#define XSC_UPTIME      40
#define XSC_EXECV       60   /* execve with proper char **argv */

/*  raw inline syscall helpers  */
static inline __x_i64 _xsc0(__x_u64 n) {
    __x_i64 r;
    __asm__ volatile("syscall"
        : "=a"(r) : "a"(n) : "rcx","r11","memory");
    return r;
}
static inline __x_i64 _xsc1(__x_u64 n, __x_u64 a) {
    __x_i64 r;
    __asm__ volatile("syscall"
        : "=a"(r) : "a"(n),"D"(a) : "rcx","r11","memory");
    return r;
}
static inline __x_i64 _xsc2(__x_u64 n, __x_u64 a, __x_u64 b) {
    __x_i64 r;
    __asm__ volatile("syscall"
        : "=a"(r) : "a"(n),"D"(a),"S"(b) : "rcx","r11","memory");
    return r;
}
static inline __x_i64 _xsc3(__x_u64 n, __x_u64 a, __x_u64 b, __x_u64 c) {
    __x_i64 r;
    __asm__ volatile("syscall"
        : "=a"(r) : "a"(n),"D"(a),"S"(b),"d"(c) : "rcx","r11","memory");
    return r;
}
static inline __x_i64 _xsc6(__x_u64 n,
    __x_u64 a, __x_u64 b, __x_u64 c,
    __x_u64 d, __x_u64 e, __x_u64 f)
{
    __x_i64 r;
    register __x_u64 r10 __asm__("r10") = d;
    register __x_u64 r8  __asm__("r8")  = e;
    register __x_u64 r9  __asm__("r9")  = f;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9)
        : "rcx","r11","memory");
    return r;
}

/*  Linux x86-64 syscall numbers  */
#define LSC_read             0
#define LSC_write            1
#define LSC_open             2
#define LSC_close            3
#define LSC_stat             4
#define LSC_fstat            5
#define LSC_lseek            8
#define LSC_mmap             9
#define LSC_mprotect        10
#define LSC_munmap          11
#define LSC_brk             12
#define LSC_ioctl           16
#define LSC_pread64         17
#define LSC_pwrite64        18
#define LSC_writev          20
#define LSC_access          21
#define LSC_sched_yield     24
#define LSC_dup             32
#define LSC_dup2            33
#define LSC_nanosleep       35
#define LSC_getpid          39
#define LSC_socket          41
#define LSC_connect         42
#define LSC_accept          43
#define LSC_sendto          44
#define LSC_recvfrom        45
#define LSC_bind            49
#define LSC_listen          50
#define LSC_fork            57
#define LSC_execve          59
#define LSC_exit            60
#define LSC_wait4           61
#define LSC_fcntl           72
#define LSC_uname           63
#define LSC_getcwd          79
#define LSC_chdir           80
#define LSC_mkdir           83
#define LSC_unlink          87
#define LSC_getuid         102
#define LSC_getgid         104
#define LSC_geteuid        107
#define LSC_getegid        108
#define LSC_rt_sigqueueinfo 129
#define LSC_arch_prctl     158
#define LSC_futex          202
#define LSC_getdents64     217
#define LSC_set_tid_addr   218
#define LSC_clock_gettime  228
#define LSC_exit_group     231
#define LSC_set_robust_list 273

/* SYS_futex and SYS_rt_sigqueueinfo are provided by bits/syscall.h which is
 * included in src/internal/syscall.h after this file.  Do NOT redefine them
 * here — it causes harmless-but-noisy redefinition warnings on every TU. */

/*  internal seek constants  */
#define _XSK_SET 0
#define _XSK_CUR 1
#define _XSK_END 2

/*  minimal stat / utsname layouts  */
typedef struct { __x_u64 size; __x_u32 type; __x_u64 inode; } _xstat_t;
typedef struct { char s[65],n[65],r[65],v[65],m[65]; } _xutsname_t;

/*  iovec (for writev)  */
typedef struct { __x_u64 base; __x_u64 len; } _xiov_t;

/*  main dispatcher  */
static inline __x_i64 __exploidus_syscall(
    __x_u64 nr,
    __x_u64 a0, __x_u64 a1, __x_u64 a2,
    __x_u64 a3, __x_u64 a4, __x_u64 a5)
{
    (void)a3; (void)a4; (void)a5;

    switch (nr) {

    /* ── I/O ── */
    case LSC_read:
        return _xsc3(XSC_READ, a0, a1, a2);

    case LSC_write:
        return _xsc3(XSC_WRITE, a0, a1, a2);

    case LSC_writev: {
        _xiov_t *v = (_xiov_t *)(__x_uptr)a1;
        __x_i64 total = 0;
        for (__x_u64 i = 0; i < a2; i++)
            total += _xsc3(XSC_WRITE, a0, v[i].base, v[i].len);
        return total;
    }

    case LSC_pread64: {
        __x_i64 saved = _xsc3(XSC_LSEEK, a0, 0, _XSK_CUR);
        _xsc3(XSC_LSEEK, a0, a3, _XSK_SET);
        __x_i64 n = _xsc3(XSC_READ, a0, a1, a2);
        _xsc3(XSC_LSEEK, a0, (__x_u64)saved, _XSK_SET);
        return n;
    }

    case LSC_pwrite64: {
        __x_i64 saved = _xsc3(XSC_LSEEK, a0, 0, _XSK_CUR);
        _xsc3(XSC_LSEEK, a0, a3, _XSK_SET);
        __x_i64 n = _xsc3(XSC_WRITE, a0, a1, a2);
        _xsc3(XSC_LSEEK, a0, (__x_u64)saved, _XSK_SET);
        return n;
    }

    /* ── files ── */
    case LSC_open:
        return _xsc2(XSC_OPEN, a0, a1);

    case LSC_close:
        return _xsc1(XSC_CLOSE, a0);

    case LSC_lseek:
        return _xsc3(XSC_LSEEK, a0, a1, a2);

    case LSC_stat: {
        _xstat_t st;
        __x_i64 r = _xsc2(XSC_STAT, a0, (__x_u64)(__x_uptr)&st);
        if (r == 0 && a1) {
            __x_u64 *ls = (__x_u64 *)(__x_uptr)a1;
            for (int i = 0; i < 18; i++) ls[i] = 0;
            ls[1] = st.inode;
            ls[6] = st.size;
        }
        return r;
    }

    case LSC_fstat: {
        _xstat_t st;
        __x_i64 r = _xsc2(XSC_FSTAT, a0, (__x_u64)(__x_uptr)&st);
        if (r == 0 && a1) {
            __x_u64 *ls = (__x_u64 *)(__x_uptr)a1;
            for (int i = 0; i < 18; i++) ls[i] = 0;
            ls[1] = st.inode;
            ls[6] = st.size;
        }
        return r;
    }

    case LSC_dup:
        return _xsc1(XSC_DUP, a0);

    case LSC_dup2:
        return _xsc2(XSC_DUP2, a0, a1);

    case LSC_getcwd:
        return _xsc2(XSC_GETCWD, a0, a1);

    case LSC_chdir:
        return _xsc1(XSC_CHDIR, a0);

    case LSC_mkdir:
        return _xsc2(XSC_CREATE, a0, 1);

    case LSC_unlink:
        return _xsc1(XSC_UNLINK, a0);

    case LSC_getdents64:
        return _xsc3(XSC_READDIR, a0, a1, a2);

    case LSC_fcntl:
    case LSC_ioctl:
    case LSC_access:
        return 0;

    /*  memory  */
    case LSC_mmap:
        return _xsc2(XSC_MMAP, 0, a1);

    case LSC_munmap:
        return _xsc2(XSC_MUNMAP, a0, a1);

    case LSC_mprotect:
    case LSC_brk:
        return 0;

    /*  process  */
    case LSC_fork:
        return _xsc0(XSC_FORK);

    case LSC_execve:
        /* a0=path  a1=argv(char**)  a2=envp — SYS_EXECV handles argv */
        return _xsc3(XSC_EXECV, a0, a1, a2);

    case LSC_exit:
    case LSC_exit_group:
        _xsc1(XSC_EXIT, a0);
        for (;;) {}

    case LSC_wait4:
        return _xsc1(XSC_WAITPID, a0);

    case LSC_getpid:
        return _xsc0(XSC_GETPID);

    case LSC_sched_yield:
        return _xsc0(XSC_YIELD);

    case LSC_nanosleep: {
        __x_u64 *ts = (__x_u64 *)(__x_uptr)a0;
        __x_u64 ticks = ts[0] * 100 + ts[1] / 10000000;
        return _xsc1(XSC_SLEEP, ticks ? ticks : 1);
    }

    /*  network  */
    case LSC_socket:
        /* a0=domain a1=type(SOCK_STREAM=1/SOCK_DGRAM=2) a2=proto */
        return _xsc6(XSC_SOCKET, 0, 0, a1, 0, 0, 0);

    case LSC_bind: {
        __x_u16 port = 0;
        if (a1) {
            __x_u8 *sa = (__x_u8 *)(__x_uptr)a1;
            port = (__x_u16)((sa[2] << 8) | sa[3]);
        }
        return _xsc6(XSC_BIND, 0, 0, a0, (__x_u64)port, 0, 0);
    }

    case LSC_connect: {
        __x_u32 ip = 0; __x_u16 port = 0;
        if (a1) {
            __x_u8 *sa = (__x_u8 *)(__x_uptr)a1;
            port = (__x_u16)((sa[2] << 8) | sa[3]);
            ip   = ((__x_u32)sa[4] << 24) | ((__x_u32)sa[5] << 16)
                 | ((__x_u32)sa[6] <<  8) |  (__x_u32)sa[7];
        }
        return _xsc6(XSC_CONNECT, 0, 0, a0, (__x_u64)ip, (__x_u64)port, 0);
    }

    case LSC_listen:
        return _xsc6(XSC_LISTEN, 0, 0, a0, 0, 0, 0);

    case LSC_accept:
        return _xsc6(XSC_ACCEPT, 0, 0, a0, 0, 0, 0);

    case LSC_sendto:
        return _xsc3(XSC_NET_SEND, a0, a1, a2);

    case LSC_recvfrom:
        return _xsc3(XSC_NET_RECV, a0, a1, a2);

    /* ── identity (single-user OS — always root) ── */
    case LSC_getuid: case LSC_geteuid:
    case LSC_getgid: case LSC_getegid:
        return 0;

    /* ── uname ── */
    case LSC_uname: {
        _xutsname_t *u = (_xutsname_t *)(__x_uptr)a0;
        if (!u) return -1;
        const char *f[] = { "Exploidus","exploidus","0.1.0","#1","x86_64" };
        char *d[] = { u->s, u->n, u->r, u->v, u->m };
        for (int i = 0; i < 5; i++) {
            int j = 0;
            while (f[i][j] && j < 64) { d[i][j] = f[i][j]; j++; }
            d[i][j] = 0;
        }
        return 0;
    }

    /*  musl threading stubs (Exploidus is single-threaded)  */
    case LSC_arch_prctl:
    case LSC_set_tid_addr:
    case LSC_set_robust_list:
    case LSC_futex:
    case LSC_rt_sigqueueinfo:
        return 0;

    case LSC_clock_gettime: {
        __x_u64 *ts = (__x_u64 *)(__x_uptr)a1;
        if (ts) { ts[0] = (__x_u64)_xsc0(XSC_UPTIME); ts[1] = 0; }
        return 0;
    }

    default:
        return -38; /* ENOSYS */
    }
}

#endif /* __EXPLOIDUS_MUSL_SYSCALL_H */
SHIM_EOF

#  step 2.5: stub misc files that use unsupported Linux syscalls 
# These functions (getpriority, getrlimit, getresuid, getresgid, setpriority,
# setrlimit) all call syscall() with SYS_* constants that don't exist on
# Exploidus.  Replace them with safe no-op stubs — Exploidus is single-user
# (uid=0) and has no resource limits.
echo "[2.5/5] Stubbing unsupported POSIX misc files..."

cat > src/misc/getpriority.c << 'STUB_EOF'
#include <sys/resource.h>
int getpriority(int which, unsigned who) { (void)which; (void)who; return 0; }
STUB_EOF

cat > src/misc/setpriority.c << 'STUB_EOF'
#include <sys/resource.h>
int setpriority(int which, unsigned who, int prio)
{ (void)which; (void)who; (void)prio; return 0; }
STUB_EOF

cat > src/misc/getrlimit.c << 'STUB_EOF'
#include <sys/resource.h>
int getrlimit(int res, struct rlimit *r) {
    (void)res;
    if (r) { r->rlim_cur = RLIM_INFINITY; r->rlim_max = RLIM_INFINITY; }
    return 0;
}
int setrlimit(int res, const struct rlimit *r) { (void)res; (void)r; return 0; }
STUB_EOF

cat > src/misc/setrlimit.c << 'STUB_EOF'
#include <sys/resource.h>
int prlimit64(int pid, int res, const struct rlimit *new, struct rlimit *old) {
    (void)pid; (void)res; (void)new;
    if (old) { old->rlim_cur = RLIM_INFINITY; old->rlim_max = RLIM_INFINITY; }
    return 0;
}
STUB_EOF

cat > src/misc/getresuid.c << 'STUB_EOF'
#define _GNU_SOURCE
#include <unistd.h>
int getresuid(unsigned *r, unsigned *e, unsigned *s) {
    if (r) *r = 0; if (e) *e = 0; if (s) *s = 0; return 0;
}
STUB_EOF

cat > src/misc/getresgid.c << 'STUB_EOF'
#define _GNU_SOURCE
#include <unistd.h>
int getresgid(unsigned *r, unsigned *e, unsigned *s) {
    if (r) *r = 0; if (e) *e = 0; if (s) *s = 0; return 0;
}
STUB_EOF

cat > src/misc/ioctl.c << 'STUB_EOF'
#include <sys/ioctl.h>
#include <stdarg.h>
int ioctl(int fd, int req, ...) { (void)fd; (void)req; return 0; }
STUB_EOF

cat > src/misc/syscall.c << 'STUB_EOF'
#include "../internal/syscall.h"
#include <stdarg.h>
long syscall(long n, ...)
{
    va_list ap; long a,b,c,d,e,f;
    va_start(ap, n);
    a=va_arg(ap,long); b=va_arg(ap,long); c=va_arg(ap,long);
    d=va_arg(ap,long); e=va_arg(ap,long); f=va_arg(ap,long);
    va_end(ap);
    return __syscall6(n,a,b,c,d,e,f);
}
STUB_EOF

#  step 2.6: pre-generate bits/syscall.h so SYS_* constants exist 
echo "[2.6/5] Pre-generating obj/include/bits/syscall.h ..."
mkdir -p obj/include/bits
sed -n 's/__NR_/SYS_/p' arch/x86_64/bits/syscall.h.in > obj/include/bits/syscall.h

#  step 3: patch src/internal/syscall.h 
echo "[3/5] Patching musl-${MUSL_VER}/src/internal/syscall.h ..."
cat > src/internal/syscall.h << PATCH_EOF
#pragma once
/*
 * Exploidus syscall bridge — replaces musl's Linux syscall layer.
 */
#include "../../../../userspace/musl/musl_syscall.h"

/* All Linux SYS_* constants — generated from arch/x86_64/bits/syscall.h.in.
 * This is what musl source files use when they call __syscall(SYS_foo, ...).
 * Unknown syscalls fall through to -ENOSYS in our dispatcher. */
#include <bits/syscall.h>

/* arity-specific variants — musl calls these directly in some files */
static inline long __syscall0(long n)
{ return (long)__exploidus_syscall((__x_u64)n,0,0,0,0,0,0); }
static inline long __syscall1(long n, long a)
{ return (long)__exploidus_syscall((__x_u64)n,(__x_u64)a,0,0,0,0,0); }
static inline long __syscall2(long n, long a, long b)
{ return (long)__exploidus_syscall((__x_u64)n,(__x_u64)a,(__x_u64)b,0,0,0,0); }
static inline long __syscall3(long n, long a, long b, long c)
{ return (long)__exploidus_syscall((__x_u64)n,(__x_u64)a,(__x_u64)b,(__x_u64)c,0,0,0); }
static inline long __syscall4(long n, long a, long b, long c, long d)
{ return (long)__exploidus_syscall((__x_u64)n,(__x_u64)a,(__x_u64)b,(__x_u64)c,(__x_u64)d,0,0); }
static inline long __syscall5(long n, long a, long b, long c, long d, long e)
{ return (long)__exploidus_syscall((__x_u64)n,(__x_u64)a,(__x_u64)b,(__x_u64)c,(__x_u64)d,(__x_u64)e,0); }
static inline long __syscall6(long n, long a, long b, long c, long d, long e, long f)
{ return (long)__exploidus_syscall((__x_u64)n,(__x_u64)a,(__x_u64)b,(__x_u64)c,(__x_u64)d,(__x_u64)e,(__x_u64)f); }

/* variadic dispatch — picks arity at compile time */
#define __NARG_X(a,b,c,d,e,f,g,h,...) h
#define __NARG(...)  __NARG_X(__VA_ARGS__, 6,5,4,3,2,1,0,)
#define __CAT(a,b)   a##b
#define __SC(n,...)  __CAT(__syscall,n)(__VA_ARGS__)
#define __syscall(...) __SC(__NARG(__VA_ARGS__), __VA_ARGS__)

/* __syscall_ret: convert negative kernel errno to -1+errno (POSIX).
 * Defined in src/internal/syscall_ret.c — declared here for all callers. */
long __syscall_ret(unsigned long r);

/* syscall(): public variadic wrapper (defined in src/unistd/syscall.c).
 * Some src/misc/ and src/sched/ files call this directly. */
extern long syscall(long, ...);

/* __sys_open / __sys_open_cp: used by __libc_start_main.c and other musl
 * internals.  Original musl syscall.h provides these; we must too. */
#define __sys_open(...)     __syscall(SYS_open, __VA_ARGS__)
#define __sys_open_cp(...)  __syscall(SYS_open, __VA_ARGS__)

/* syscall_arg_t: the integer type used for syscall arguments.
 * SYSCALL_MMAP2_UNIT: page-size unit for mmap2 offset argument.
 * syscall_cp: cancellation-point syscall — no threads on Exploidus,
 *             so just alias to the regular __syscall. */
typedef long syscall_arg_t;
#define SYSCALL_MMAP2_UNIT 4096ULL
#define syscall_cp __syscall
PATCH_EOF

#  step 4: configure 
echo "[4/5] Configuring..."
make clean -s 2>/dev/null || true

./configure \
    --prefix="${SYSROOT}" \
    --syslibdir="${SYSROOT}/lib" \
    --disable-shared \
    --enable-static \
    CC="${CROSS}-gcc" \
    CFLAGS="-ffreestanding -fno-stack-protector -fno-pie \
            -mno-red-zone \
            -Wno-int-conversion \
            -D_GNU_SOURCE \
            -I${REPO}/userspace/musl \
            -nostdinc -O2" \
    2>&1 | grep -v "^$" | tail -8

#  step 5: build 
echo "[5/5] Building (≈2 min)..."
make -j"$(nproc)"
make install -s

echo ""
echo "========================================"
echo " musl build complete!"
echo " sysroot: ${SYSROOT}"
echo "========================================"
echo ""
echo " Test compile:"
echo "   ${CROSS}-gcc -static -nostdlib \\"
echo "     -I${SYSROOT}/include \\"
echo "     -L${SYSROOT}/lib \\"
echo "     -o hello.elf hello.c \\"
echo "     ${SYSROOT}/lib/libc.a"