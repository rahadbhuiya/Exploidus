# Exploidus musl Compatibility Layer

`userspace/musl/` is Exploidus's Linux software compatibility layer.
It lets musl-compiled C programs run on Exploidus without modification.

---

## How it works

```
Linux software (C source)
        ↓  compiled with musl + exploidus arch
   musl libc
        ↓  calls __exploidus_syscall() instead of Linux syscall
   musl_syscall.h  ← maps Linux syscall numbers → Exploidus syscalls
        ↓
   Exploidus kernel
```

---

## Building musl for Exploidus

### 1. Download musl
```bash
wget https://musl.libc.org/releases/musl-1.2.5.tar.gz
tar -xzf musl-1.2.5.tar.gz
cd musl-1.2.5
```

### 2. Patch the syscall entry point
musl calls Linux syscalls via `src/internal/syscall.h`.
Replace the syscall macro to redirect to our shim:

In `src/internal/syscall.h`, replace:
```c
static inline long __syscall(long n, ...)
```
with:
```c
#include "../../../../userspace/musl/musl_syscall.h"
#define __syscall(n, a, b, c, d, e, f) \
    __exploidus_syscall((uint64_t)(n), (uint64_t)(a), (uint64_t)(b), \
                        (uint64_t)(c), (uint64_t)(d), (uint64_t)(e), \
                        (uint64_t)(f))
```

### 3. Configure musl
```bash
./configure \
  --prefix=/exploidus-sysroot \
  --target=x86_64-exploidus \
  --disable-shared \
  --enable-static \
  CFLAGS="-ffreestanding -fno-stack-protector -fpie \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
          -Iuserspace/musl"
```

### 4. Build
```bash
make -j$(nproc)
make install
```

### 5. Compile a Linux program for Exploidus
```bash
x86_64-elf-gcc \
  -static \
  -fpie \
  -ffreestanding \
  -nostdlib \
  -L/exploidus-sysroot/lib \
  -I/exploidus-sysroot/include \
  -o myprogram.elf \
  myprogram.c \
  /exploidus-sysroot/lib/libc.a \
  userspace/libc/crt0.o
```

---

## Syscall coverage

| Linux syscall     | Exploidus mapping          | Status |
|-------------------|----------------------------|--------|
| read              | SYS_READ                   | Done     |
| write             | SYS_WRITE                  | Done     |
| open              | SYS_OPEN                   | Done     |
| close             | SYS_CLOSE                  | Done     |
| mmap              | SYS_MMAP                   | Done     |
| munmap            | SYS_MUNMAP                 | Done     |
| fork              | SYS_FORK                   | Done     |
| execve            | SYS_SPAWN                  | Done     |
| exit / exit_group | SYS_EXIT                   | Done     |
| wait4             | SYS_WAITPID                | Done     |
| getpid            | SYS_GETPID                 | Done     |
| sched_yield       | SYS_YIELD                  | Done     |
| nanosleep         | SYS_SLEEP (ticks)          | Done     |
| socket            | SYS_SOCKET                 | Done     |
| bind              | SYS_BIND                   | Done     |
| connect           | SYS_CONNECT                | Done     |
| listen            | SYS_LISTEN                 | Done     |
| accept            | SYS_ACCEPT                 | Done     |
| sendto            | SYS_NET_SEND               | Done     |
| recvfrom          | SYS_NET_RECV               | Done     |
| getcwd            | SYS_GETCWD                 | Done     |
| chdir             | SYS_CHDIR                  | Done     |
| getdents64        | SYS_READDIR                | Done     |
| uname             | returns "Exploidus"        | Done     |
| getuid/getgid     | returns 0 (root)           | Done     |
| arch_prctl        | no-op (TLS stub)           | Done     |
| set_tid_addr      | returns getpid()           | Done     |
| futex             | no-op (single-threaded)    | Done     |
| clock_gettime     | SYS_UPTIME                 | Done     |
| mprotect          | no-op                      | Done     |
| brk               | returns 0 (uses mmap)      | Done     |
| writev            | flattened write loop       | Done     |
| ioctl             | no-op                      | Done     |
| fcntl             | no-op                      | Done     |
| access            | always returns 0           | Done     |
| lseek             | -ENOSYS                    | Pending  |
| stat / fstat      | -ENOSYS                    | Pending  |
| dup / dup2        | -ENOSYS                    | Pending  |
| pread64/pwrite64  | -ENOSYS                    | Pending  |

---

## What works now
- Any single-threaded musl program that uses stdio, malloc, string functions
- Network programs using BSD socket API
- File I/O programs (read/write/open/close)

## Not yet supported
- `lseek` — needs VFS seek implementation
- `stat/fstat` — needs VFS stat implementation  
- `dup/dup2` — needs fd table cloning
- Multithreading — futex is a no-op
- argv/envp passing to execve

## Author
Rahad Bhuiya — Exploidus Project