#pragma once
/* Use local stdint — must come before any typedef */
#ifdef __EXPLOIDUS_USERSPACE__
#  include "stdint.h"
#else
#  include <stdint.h>
#endif
#include "../../kernel/syscall/table.h"

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
#define SYS_UPTIME       40
#define SYS_PING         41

#ifndef _SYSCALL_H_TYPES_DEFINED
#define _SYSCALL_H_TYPES_DEFINED
typedef uint64_t size_t;
typedef int64_t  ssize_t;
#endif
typedef uint32_t ip4_t;

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SOCK_TCP  1
#define SOCK_UDP  2

#define IP4(a,b,c,d) \
    (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))


/*  Raw syscall stubs  */

static inline int64_t syscall0(uint64_t n)
{
    int64_t r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "a"(n) : "rcx","r11","memory");
    return r;
}
static inline int64_t syscall1(uint64_t n, uint64_t a)
{
    int64_t r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "a"(n),"D"(a) : "rcx","r11","memory");
    return r;
}
static inline int64_t syscall2(uint64_t n, uint64_t a, uint64_t b)
{
    int64_t r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "a"(n),"D"(a),"S"(b) : "rcx","r11","memory");
    return r;
}
static inline int64_t syscall3(uint64_t n, uint64_t a, uint64_t b, uint64_t c)
{
    int64_t r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "a"(n),"D"(a),"S"(b),"d"(c) : "rcx","r11","memory");
    return r;
}
static inline int64_t syscall6(uint64_t n,
                                uint64_t a, uint64_t b, uint64_t c,
                                uint64_t d, uint64_t e, uint64_t g)
{
    int64_t r;
    register uint64_t r10 __asm__("r10") = d;
    register uint64_t r8  __asm__("r8")  = e;
    register uint64_t r9  __asm__("r9")  = g;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9)
        : "rcx","r11","memory");
    return r;
}

/*  Process  */
#ifndef __EXPLOIDUS_LIBC__
static inline void exit(int code)
{
    syscall1(SYS_EXIT,(uint64_t)(int64_t)code); for(;;){}
}
#endif
static inline int64_t fork(void)          { return syscall0(SYS_FORK); }
static inline int64_t getpid(void)        { return syscall0(SYS_GETPID); }
static inline int64_t yield(void)         { return syscall0(SYS_YIELD); }
static inline int64_t waitpid(int64_t p)  { return syscall1(SYS_WAITPID,(uint64_t)p); }
static inline int64_t sleep_ticks(uint64_t t) { return syscall1(SYS_SLEEP,t); }

/*  File I/O  */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200

static inline int open(const char *path, int flags)
{
    return (int)syscall2(SYS_OPEN,(uint64_t)(uintptr_t)path,(uint64_t)flags);
}
static inline int close(int fd) { return (int)syscall1(SYS_CLOSE,(uint64_t)(int64_t)fd); }

/*  I/O  */
static inline ssize_t write(int fd, const void *buf, size_t n)
{
    return (ssize_t)syscall3(SYS_WRITE,
        (uint64_t)(int64_t)fd,(uint64_t)(uintptr_t)buf,(uint64_t)n);
}
static inline ssize_t read(int fd, void *buf, size_t n)
{
    return (ssize_t)syscall3(SYS_READ,
        (uint64_t)(int64_t)fd,(uint64_t)(uintptr_t)buf,(uint64_t)n);
}

/*  Memory  */
static inline void *mmap(size_t len)
{
    int64_t r = syscall2(SYS_MMAP, 0, (uint64_t)len);
    return (r < 0) ? (void *)0 : (void *)(uintptr_t)(uint64_t)r;
}
static inline int munmap(void *addr, size_t len)
{
    return (int)syscall2(SYS_MUNMAP,(uint64_t)(uintptr_t)addr,(uint64_t)len);
}

/*  Audit log  */
typedef struct { uint64_t timestamp; uint32_t pid; uint32_t event;
                 uint64_t arg0; uint64_t arg1; } audit_entry_user_t;

static inline int64_t audit_dump(cap_token_t cap,
                                  audit_entry_user_t *buf, uint64_t max)
{
    register uint64_t r10 __asm__("r10") = max;
    int64_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "a"((uint64_t)SYS_AUDIT_DUMP),
          "D"(cap.upper),"S"(cap.lower),"d"((uint64_t)(uintptr_t)buf),
          "r"(r10)
        : "rcx","r11","memory");
    return r;
}

/*  Process table  */
typedef struct { uint32_t pid; uint32_t parent_pid;
                 uint32_t state; uint32_t intent; uint64_t ticks_used; } proc_info_t;

static inline int64_t getprocs(proc_info_t *buf, uint64_t max)
{
    return syscall2(SYS_GETPROCS,(uint64_t)(uintptr_t)buf,max);
}

/*  Network  */
static inline int xsocket(cap_token_t cap, int type)
{
    return (int)syscall6(SYS_SOCKET,cap.upper,cap.lower,(uint64_t)type,0,0,0);
}
static inline int xbind(cap_token_t cap, int fd, uint16_t port)
{
    return (int)syscall6(SYS_BIND,cap.upper,cap.lower,(uint64_t)fd,(uint64_t)port,0,0);
}
static inline int xconnect(cap_token_t cap, int fd, ip4_t ip, uint16_t port)
{
    return (int)syscall6(SYS_CONNECT,cap.upper,cap.lower,(uint64_t)fd,(uint64_t)ip,(uint64_t)port,0);
}
static inline int xlisten(cap_token_t cap, int fd)
{
    return (int)syscall6(SYS_LISTEN,cap.upper,cap.lower,(uint64_t)fd,0,0,0);
}
static inline int xaccept(cap_token_t cap, int fd)
{
    return (int)syscall6(SYS_ACCEPT,cap.upper,cap.lower,(uint64_t)fd,0,0,0);
}
static inline int xsend(cap_token_t cap, int fd, const void *buf, uint16_t len)
{
    return (int)syscall6(SYS_NET_SEND,cap.upper,cap.lower,(uint64_t)fd,
        (uint64_t)(uintptr_t)buf,(uint64_t)len,0);
}
static inline int xrecv(cap_token_t cap, int fd, void *buf, uint16_t len)
{
    return (int)syscall6(SYS_NET_RECV,cap.upper,cap.lower,(uint64_t)fd,
        (uint64_t)(uintptr_t)buf,(uint64_t)len,0);
}
static inline void xclose(int fd)
{
    syscall3(SYS_NET_CLOSE,0,0,(uint64_t)fd);
}
static inline ip4_t getifaddr(void) { return (ip4_t)syscall0(SYS_GETIFADDR); }

/*  String helpers  */
#ifndef __EXPLOIDUS_LIBC__
static inline size_t strlen(const char *s)
    { size_t n=0; while(*s++)n++; return n; }
#ifndef _STDIO_H_
static inline void puts(const char *s)
    { write(STDOUT_FILENO,s,strlen(s)); }
static inline void putc(char c)
    { write(STDOUT_FILENO,&c,1); }
#endif
#endif
static inline void *memset_u(void *d,int v,size_t n)
    { uint8_t *p=(uint8_t*)d; while(n--)*p++=(uint8_t)v; return d; }
static inline int strcmp_u(const char *a,const char *b)
    { while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }

#define SYS_FB_PIXEL  29
#define SYS_FB_RECT   30
#define SYS_FB_CLEAR  31
#define SYS_FB_INFO   34
#define SYS_MOUSE_POS 37
#define SYS_FB_STR    38
#define SYS_READDIR   35
#define SYS_CREATE    36

typedef struct {
    uint64_t inode;
    uint8_t  type;
    char     name[256];
} dirent_t;

static inline int64_t readdir(int fd, dirent_t *buf, uint64_t max)
{
    return syscall3(SYS_READDIR, (uint64_t)fd,
                    (uint64_t)(uintptr_t)buf, max);
}
static inline int fs_create(const char *path, int type)
{
    return (int)syscall2(SYS_CREATE,
                         (uint64_t)(uintptr_t)path, (uint64_t)type);
}

/*  Framebuffer  */
static inline void fb_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    syscall3(SYS_FB_PIXEL, (uint64_t)x, (uint64_t)y, (uint64_t)color);
}
static inline void fb_rect(uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h, uint32_t color)
{
    register uint64_t r10 __asm__("r10") = (uint64_t)h;
    register uint64_t r8  __asm__("r8")  = (uint64_t)color;
    int64_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "a"((uint64_t)SYS_FB_RECT),
          "D"((uint64_t)x), "S"((uint64_t)y),
          "d"((uint64_t)w), "r"(r10), "r"(r8)
        : "rcx","r11","memory");
}
static inline void fb_clear(uint32_t color)
{
    syscall1(SYS_FB_CLEAR, (uint64_t)color);
}
static inline int fb_info(uint32_t *w, uint32_t *h, uint32_t *active)
{
    uint32_t buf[4];
    int64_t r = syscall1(SYS_FB_INFO, (uint64_t)(uintptr_t)buf);
    *w = buf[0]; *h = buf[1]; *active = buf[2];
    return (int)r;
}
static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static inline void fb_str(uint32_t x, uint32_t y, const char *s,
                           uint32_t fg, uint32_t bg)
{
    register uint64_t r10 __asm__("r10") = (uint64_t)fg;
    register uint64_t r8  __asm__("r8")  = (uint64_t)bg;
    int64_t r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "a"((uint64_t)SYS_FB_STR),
          "D"((uint64_t)x), "S"((uint64_t)y),
          "d"((uint64_t)(uintptr_t)s),
          "r"(r10), "r"(r8)
        : "rcx","r11","memory");
}
static inline void mouse_pos(int32_t *x, int32_t *y, int32_t *btn)
{
    int32_t buf[3];
    syscall1(SYS_MOUSE_POS, (uint64_t)(uintptr_t)buf);
    *x = buf[0]; *y = buf[1]; *btn = buf[2];
}
#define SYS_POWEROFF  32
#define SYS_REBOOT    33
static inline uint64_t uptime(void) { return (uint64_t)syscall0(SYS_UPTIME); }
static inline void poweroff(void) { syscall0(SYS_POWEROFF); }
static inline void reboot(void)   { syscall0(SYS_REBOOT); }

#define SYS_SPAWN     39
static inline int64_t spawn(const char *path)
{
    return syscall1(SYS_SPAWN, (uint64_t)(uintptr_t)path);
}
static inline int64_t spawn_args(const char *path, const char *args)
{
    return syscall3(SYS_SPAWN, (uint64_t)(uintptr_t)path, 3, (uint64_t)(uintptr_t)args);
}
static inline int64_t spawn_intent(const char *path, uint64_t intent)
{
    return syscall2(SYS_SPAWN, (uint64_t)(uintptr_t)path, intent);
}

/*  lseek / stat / dup  */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

static inline int64_t lseek(int fd, int64_t offset, int whence)
{
    return syscall3(SYS_LSEEK, (uint64_t)(int64_t)fd,
                    (uint64_t)offset, (uint64_t)(int64_t)whence);
}

typedef struct {
    uint64_t size;
    uint32_t type;
    uint64_t inode;
} vfs_stat_t;

static inline int stat(const char *path, vfs_stat_t *st)
{
    return (int)syscall2(SYS_STAT,
        (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)st);
}
static inline int fstat(int fd, vfs_stat_t *st)
{
    return (int)syscall2(SYS_FSTAT,
        (uint64_t)(int64_t)fd, (uint64_t)(uintptr_t)st);
}
static inline int dup(int fd)
{
    return (int)syscall1(SYS_DUP, (uint64_t)(int64_t)fd);
}
static inline int dup2(int oldfd, int newfd)
{
    return (int)syscall2(SYS_DUP2,
        (uint64_t)(int64_t)oldfd, (uint64_t)(int64_t)newfd);
}

/*  rahu package manager syscalls  */
#define SYS_HTTP_GET    55
#define SYS_FILE_WRITE  56
#define SYS_BLAKE3      57
#define SYS_UNLINK      58

static inline int64_t http_get(const char *url, void *buf, uint64_t size)
{
    return syscall3(SYS_HTTP_GET,
        (uint64_t)(uintptr_t)url,
        (uint64_t)(uintptr_t)buf,
        size);
}
static inline int64_t file_write(const char *path, const void *data, uint64_t size)
{
    return syscall3(SYS_FILE_WRITE,
        (uint64_t)(uintptr_t)path,
        (uint64_t)(uintptr_t)data,
        size);
}
static inline int64_t blake3(const void *buf, uint64_t len, uint8_t *out)
{
    return syscall3(SYS_BLAKE3,
        (uint64_t)(uintptr_t)buf,
        len,
        (uint64_t)(uintptr_t)out);
}
static inline int unlink(const char *path)
{
    return (int)syscall1(SYS_UNLINK, (uint64_t)(uintptr_t)path);
}
static inline int64_t http_download(const char *url, const char *dest_path, uint8_t *hash_out)
{
    return syscall3(SYS_HTTP_DOWNLOAD,
        (uint64_t)(uintptr_t)url,
        (uint64_t)(uintptr_t)dest_path,
        (uint64_t)(uintptr_t)hash_out);
}

static inline int64_t ping(ip4_t ip) { return syscall1(SYS_PING, (uint64_t)ip); }

#define SYS_FB_CIRCLE  42
#define SYS_FB_RRECT   43
#define SYS_FB_FLIP    44
#define SYS_CHDIR      45
#define SYS_GETCWD     46

static inline void fb_circle(int cx, int cy, int r, uint32_t color)
{
    register uint64_t r10 __asm__("r10") = (uint64_t)color;
    int64_t ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"((uint64_t)SYS_FB_CIRCLE),
          "D"((uint64_t)(int64_t)cx),
          "S"((uint64_t)(int64_t)cy),
          "d"((uint64_t)(int64_t)r),
          "r"(r10)
        : "rcx","r11","r8","r9","memory");
}
static inline int chdir(const char *path)
{
    return (int)syscall1(SYS_CHDIR, (uint64_t)(uintptr_t)path);
}
static inline int getcwd(char *buf, uint64_t size)
{
    return (int)syscall2(SYS_GETCWD, (uint64_t)(uintptr_t)buf, size);
}

/*  CNSL security commands  */
static inline int cnsl_unblock_ip(uint32_t ip)
{
    return (int)syscall1(SYS_CNSL_UNBLOCK, (uint64_t)ip);
}
static inline uint64_t cnsl_block_ttl(uint32_t ip)
{
    return (uint64_t)syscall1(SYS_CNSL_BLOCK_TTL, (uint64_t)ip);
}
typedef struct { uint32_t ip; uint64_t ttl_secs; } cnsl_list_entry_t;
static inline int cnsl_list_ips(cnsl_list_entry_t *buf, int max)
{
    return (int)syscall2(SYS_CNSL_LIST,
                         (uint64_t)(uintptr_t)buf,
                         (uint64_t)(uint32_t)max);
}
static inline void fb_flip(void)
{
    syscall0(SYS_FB_FLIP);
}
static inline void fb_rrect(int x, int y, int w, int h, int r, uint32_t color)
{
    syscall6(SYS_FB_RRECT,
             (uint64_t)(int64_t)x, (uint64_t)(int64_t)y,
             (uint64_t)(int64_t)w, (uint64_t)(int64_t)h,
             (uint64_t)(int64_t)r, (uint64_t)color);
}

/*  GUI : IPC  */

/*
 * ipc_msg_t — userspace mirror of kernel ipc_msg_t.
 * Must match kernel/ipc/ipc.h exactly.
 */
#define IPC_MSG_PAYLOAD 120

typedef struct {
    uint32_t from_pid;              /* filled by kernel on receive        */
    uint32_t type;                  /* caller-defined message type        */
    uint8_t  data[IPC_MSG_PAYLOAD]; /* raw payload                        */
    uint32_t len;                   /* bytes valid in data[]              */
} ipc_msg_t;

/*
 * ipc_send — send msg to dst_pid.
 *   Returns  0  ok
 *           -1  dst_pid doesn't exist
 *           -2  dst inbox full
 */
static inline int64_t ipc_send(uint32_t dst_pid, ipc_msg_t *msg)
{
    return syscall2(SYS_IPC_SEND,
                    (uint64_t)dst_pid,
                    (uint64_t)(uintptr_t)msg);
}

/*
 * ipc_recv — blocking receive.
 * Fills *msg and returns 0 on success.
 * Blocks until a message arrives.
 */
static inline int64_t ipc_recv(ipc_msg_t *msg)
{
    return syscall1(SYS_IPC_RECV, (uint64_t)(uintptr_t)msg);
}

/*
 * ipc_recv_nb — non-blocking receive.
 * Returns 0 if a message was dequeued, -1 if queue was empty.
 */
static inline int64_t ipc_recv_nb(ipc_msg_t *msg)
{
    return syscall1(SYS_IPC_RECV_NB, (uint64_t)(uintptr_t)msg);
}

/*  GUI: SHM  */

/*
 * shm_create — allocate 'size' bytes of shared memory.
 * Returns shm_id (> 0) on success, 0 on failure.
 * The creator can then shm_map() it and share the id with other processes.
 */
static inline uint32_t shm_create(uint64_t size)
{
    return (uint32_t)syscall1(SYS_SHM_CREATE, size);
}

/*
 * shm_map — map shm_id into this process's address space.
 * Returns a pointer to the mapped region, or NULL on failure.
 */
static inline void *shm_map(uint32_t shm_id)
{
    int64_t r = syscall1(SYS_SHM_MAP, (uint64_t)shm_id);
    return (r <= 0) ? (void *)0 : (void *)(uintptr_t)(uint64_t)r;
}

/*
 * shm_unmap — unmap a previously mapped region.
 * 'size' must match the original size used at shm_create() time.
 */
static inline void shm_unmap(void *va, uint64_t size)
{
    syscall2(SYS_SHM_UNMAP,
             (uint64_t)(uintptr_t)va,
             size);
}

/*
 * shm_destroy — free the physical pages.
 * Only the creator can destroy. All mappings must be released first.
 * Returns 0 on success, -1 on failure.
 */
static inline int64_t shm_destroy(uint32_t shm_id)
{
    return syscall1(SYS_SHM_DESTROY, (uint64_t)shm_id);
}

/*  Common IPC message types (GUI protocol)  */
/* Apps and the compositor will use these type codes in ipc_msg_t.type */
#define IPC_MSG_PING          0x01   /* liveness check                   */
#define IPC_MSG_PONG          0x02   /* liveness reply                   */
#define IPC_MSG_WIN_CREATE    0x10   /* app → compositor: create window  */
#define IPC_MSG_WIN_DESTROY   0x11   /* app → compositor: destroy window */
#define IPC_MSG_WIN_MOVE      0x12   /* WM  → compositor: move window    */
#define IPC_MSG_WIN_RESIZE    0x13   /* WM  → compositor: resize window  */
#define IPC_MSG_WIN_FOCUS     0x14   /* WM  → app: you have focus        */
#define IPC_MSG_WIN_BLUR      0x15   /* WM  → app: focus lost            */
#define IPC_MSG_DAMAGE        0x20   /* app → compositor: region repaint */
#define IPC_MSG_KEY_DOWN      0x30   /* compositor → app: key pressed    */
#define IPC_MSG_KEY_UP        0x31   /* compositor → app: key released   */
#define IPC_MSG_MOUSE_MOVE    0x32   /* compositor → app: mouse moved    */
#define IPC_MSG_MOUSE_BTN     0x33   /* compositor → app: button event   */