#include <stdint.h>
#include "table.h"
#include "../cap/broker.h"
#include "../cap/capability.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"
#include "../proc/fork_exec.h"
#include "../audit/audit.h"
#include "../drivers/serial.h"
#include "../drivers/vga.h"
#include "../drivers/fb.h"
#include "../drivers/font.h"
#include "../drivers/mouse.h"
#include "../net/socket/socket.h"
#include "../net/net.h"
#include "../fs/vfs/vfs.h"
#include "../mm/kmalloc.h"
#include "../elf/elf.h"
extern int64_t vfs_readdir(int fd, void *buf, uint64_t max);
extern int vfs_create(const char *path, uint8_t type);
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include <string.h>

/*  MSR constants  */
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_FMASK  0xC0000084
#define EFER_SCE   (1 << 0)
#define MSR_EFER   0xC0000080

extern void syscall_entry_stub(void);

/* Userspace virtual address ceiling — anything at or above this is kernel */
#define USER_ADDR_MAX  0x00007FFFFFFFFFFFULL

/*
 * uptr_ok — validate that [addr, addr+len) lies entirely in user space.
 * Must be called before dereferencing any pointer supplied by a syscall.
 *
 * FIX: len=0 is valid (e.g. write of empty string). Only addr=0 is rejected.
 */
static bool uptr_ok(uint64_t addr, uint64_t len)
{
    if (!addr) return false;
    if (addr > USER_ADDR_MAX) return false;
    if (!len)  return true;   /* zero-length transfer is always OK */
    if (len  > USER_ADDR_MAX) return false;
    if (len - 1 > USER_ADDR_MAX - addr) return false;
    return true;
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFF);
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
    /*
     * STAR MSR layout:
     *   [63:48] = user CS - 16  (sysretq sets CS = this | 3, SS = this + 8 | 3)
     *   [47:32] = kernel CS     (syscall sets CS = this, SS = this + 8)
     *
     * Our runtime GDT:
     *   0x08 = kernel code
     *   0x10 = kernel data
     *   0x18 = user data   (sysretq: STAR[63:48] + 8  | 3 = 0x1B)
     *   0x20 = user code   (sysretq: STAR[63:48] + 16 | 3 = 0x23)
     *
     * So STAR[63:48] must be 0x0010 and STAR[47:32] must be 0x0008.
     */
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

static int64_t kernel_read(int fd, void *buf, uint64_t len)
{
    if (fd != 0) return -1;
    if (!len) return 0;
    if (!buf) return -1;
    char    *p = (char *)buf;
    uint64_t n = 0;
    while (n == 0) {
        while (n < len) {
            char c;
            if (!keyboard_read(&c)) break;
            p[n++] = c;
        }
        if (n == 0) {
            __asm__ volatile ("sti; hlt; cli" ::: "memory");
        }
    }
    return (int64_t)n;
}


/*  Process syscalls   */


static __attribute__((unused)) int64_t sys_exit(syscall_frame_t *f)
{
    int code = (int)(int64_t)f->rdi;
    if (g_current_proc) proc_exit(g_current_proc->pid, code);
    sched_yield();
    /* If no process to yield to, halt */
    __asm__ volatile("cli");
    for(;;) __asm__ volatile("hlt");
    return 0;
}
static __attribute__((unused)) int64_t sys_fork(syscall_frame_t *f)   { (void)f; return sys_fork_impl(); }
static __attribute__((unused)) int64_t sys_exec(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, f->rsi)) return -1;
    return sys_exec_impl((const uint8_t *)(uintptr_t)f->rdi, f->rsi);
}
static __attribute__((unused)) int64_t sys_waitpid(syscall_frame_t *f)
{
    return sys_waitpid_impl((uint32_t)f->rdi);
}
static __attribute__((unused)) int64_t sys_read(syscall_frame_t *f)
{
    int fd = (int)(int64_t)f->rdi;

    if (fd == 0) {
        if (!uptr_ok(f->rsi, f->rdx)) return -1;
        return kernel_read(fd, (void *)(uintptr_t)f->rsi, f->rdx);
    }
    if (!uptr_ok(f->rsi, f->rdx)) return -1;
    return vfs_read(fd, (void *)(uintptr_t)f->rsi, f->rdx);
}
static __attribute__((unused)) int64_t sys_write(syscall_frame_t *f)
{
    int fd = (int)(int64_t)f->rdi;

    if (fd == 1 || fd == 2) {
        if (!uptr_ok(f->rsi, f->rdx)) return -1;
        return kernel_write(fd, (const void *)(uintptr_t)f->rsi, f->rdx);
    }
    if (!uptr_ok(f->rsi, f->rdx)) return -1;
    return vfs_write(fd, (const void *)(uintptr_t)f->rsi, f->rdx);
}


/*  File syscalls  */


#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_CREAT   0x0040
#define O_TRUNC   0x0200
#define O_APPEND  0x0400

static __attribute__((unused)) int64_t sys_open(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    const char *path  = (const char *)(uintptr_t)f->rdi;
    uint32_t    flags = (uint32_t)f->rsi;

    uint64_t max_len = 4096;
    uint64_t i = 0;
    while (i < max_len && path[i]) i++;
    if (i == max_len) return -1;

    int fd = vfs_open(path, flags);
    if (fd < 0) {
        serial_print("[SYS] open: not found: ");
        serial_print(path);
        serial_print("\n");
    }

    if (g_current_proc)
        audit_record(AUDIT_FILE_OPEN, g_current_proc->pid, (uint64_t)(int64_t)fd, 0);

    return (int64_t)fd;
}

static __attribute__((unused)) int64_t sys_close(syscall_frame_t *f)
{
    int fd = (int)(int64_t)f->rdi;
    if (fd < 3) return -1;
    return (int64_t)vfs_close(fd);
}
static __attribute__((unused)) int64_t sys_yield(syscall_frame_t *f)  { (void)f; sched_yield(); return 0; }
static __attribute__((unused)) int64_t sys_getpid(syscall_frame_t *f) { (void)f; return g_current_proc ? (int64_t)g_current_proc->pid : -1; }
static __attribute__((unused)) int64_t sys_sleep(syscall_frame_t *f)
{
    extern uint64_t g_uptime_ticks;
    uint64_t end = g_uptime_ticks + f->rdi;
    while (g_uptime_ticks < end)
        __asm__ volatile ("sti; hlt; cli" ::: "memory");
    return 0;
}


/*  Anonymous mmap / munmap  */


#define MMAP_PAGE_SIZE  4096UL
#define MMAP_MAX_BYTES  (256UL * 1024 * 1024)

static __attribute__((unused)) int64_t sys_mmap(syscall_frame_t *f)
{
    if (!g_current_proc) return -1;

    uint64_t len = f->rsi;
    if (!len || len > MMAP_MAX_BYTES) return -1;

    len = (len + MMAP_PAGE_SIZE - 1) & ~(MMAP_PAGE_SIZE - 1);

    uint64_t pages = len / MMAP_PAGE_SIZE;

    uint64_t virt_top = g_current_proc->mmap_top;
    uint64_t virt     = virt_top - len;
    if (virt >= virt_top) return -1;
    if (virt < 0x1000)   return -1;

    for (uint64_t pg = 0; pg < pages; pg++) {
        uint64_t phys = pmm_alloc(ZONE_RED);
        if (!phys) return -1;
        memset((void *)(uintptr_t)phys, 0, MMAP_PAGE_SIZE);

        if (!vmm_map_into(g_current_proc->cr3,
                          phys,
                          virt + pg * MMAP_PAGE_SIZE,
                          VMM_WRITE | VMM_USER | VMM_NX)) {
            pmm_free(phys);
            return -1;
        }
    }

    g_current_proc->mmap_top = virt;

    serial_print("[MMAP] virt=");
    serial_printhex(virt);
    serial_print(" len=");
    serial_printhex(len);
    serial_print("\n");

    return (int64_t)virt;
}

static __attribute__((unused)) int64_t sys_munmap(syscall_frame_t *f)
{
    uint64_t addr = f->rdi;
    uint64_t len  = f->rsi;

    if (!g_current_proc || !addr || !len) return -1;
    if (addr & (MMAP_PAGE_SIZE - 1)) return -1;
    if (!uptr_ok(addr, len)) return -1;

    len = (len + MMAP_PAGE_SIZE - 1) & ~(MMAP_PAGE_SIZE - 1);
    uint64_t pages = len / MMAP_PAGE_SIZE;

    for (uint64_t pg = 0; pg < pages; pg++) {
        uint64_t virt = addr + pg * MMAP_PAGE_SIZE;
        uint64_t phys = vmm_get_phys_into(g_current_proc->cr3, virt);
        vmm_unmap_into(g_current_proc->cr3, virt);
        if (phys) pmm_free(phys);
    }
    return 0;
}


/*  Audit log dump   */


typedef struct __attribute__((packed)) {
    uint64_t timestamp;
    uint32_t pid;
    uint32_t event;
    uint64_t arg0;
    uint64_t arg1;
} audit_entry_user_t;

static __attribute__((unused)) int64_t sys_audit_dump(syscall_frame_t *f)
{
    if (!g_current_proc) return -1;
    cap_token_t tok = { f->rdi, f->rsi };
    if (!broker_check(g_current_proc->pid, tok, CAP_RIGHT_AUDIT)) return -1;

    uint64_t ubuf_ptr  = f->rdx;
    uint64_t max_count = f->r10;
    if (!max_count) return 0;
    if (max_count > USER_ADDR_MAX / sizeof(audit_entry_user_t)) return -1;
    uint64_t byte_len = max_count * sizeof(audit_entry_user_t);
    if (!uptr_ok(ubuf_ptr, byte_len)) return -1;

    uint32_t             avail = 0;
    const audit_entry_t *ring  = audit_read(&avail);
    audit_entry_user_t  *ubuf  = (audit_entry_user_t *)(uintptr_t)ubuf_ptr;

    uint32_t copy = avail < (uint32_t)max_count ? avail : (uint32_t)max_count;
    for (uint32_t i = 0; i < copy; i++) {
        ubuf[i].timestamp = ring[i].timestamp;
        ubuf[i].pid       = ring[i].pid;
        ubuf[i].event     = (uint32_t)ring[i].event;
        ubuf[i].arg0      = ring[i].arg0;
        ubuf[i].arg1      = ring[i].arg1;
    }
    return (int64_t)copy;
}


/*  Process table dump   */


typedef struct __attribute__((packed)) {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;
    uint32_t intent;
    uint64_t ticks_used;
} proc_info_t;

static __attribute__((unused)) int64_t sys_getprocs(syscall_frame_t *f)
{
    uint64_t ubuf_ptr  = f->rdi;
    uint64_t max_count = f->rsi;
    if (!max_count) return 0;
    if (max_count > USER_ADDR_MAX / sizeof(proc_info_t)) return -1;
    uint64_t byte_len = max_count * sizeof(proc_info_t);
    if (!uptr_ok(ubuf_ptr, byte_len)) return -1;

    uint32_t         table_count = 0;
    const process_t *table       = proc_get_table(&table_count);
    proc_info_t     *ubuf        = (proc_info_t *)(uintptr_t)ubuf_ptr;

    uint32_t written = 0;
    for (uint32_t i = 0; i < table_count && written < (uint32_t)max_count; i++) {
        if (table[i].state == PROC_UNUSED) continue;
        ubuf[written].pid        = table[i].pid;
        ubuf[written].parent_pid = table[i].parent_pid;
        ubuf[written].state      = (uint32_t)table[i].state;
        ubuf[written].intent     = (uint32_t)table[i].intent;
        ubuf[written].ticks_used = table[i].ticks_used;
        written++;
    }
    return (int64_t)written;
}


/*  Capability syscalls  */


static __attribute__((unused)) int64_t sys_cap_create_h(syscall_frame_t *f)
{
    if (!g_current_proc || g_current_proc->pid > 2) {
        audit_record(AUDIT_CAP_DENIED,
                     g_current_proc ? g_current_proc->pid : 0, 0, 0);
        return -1;
    }
    cap_token_t tok = cap_create((cap_resource_t)f->rdi,
                                  (uint32_t)f->rsi, f->rdx,
                                  f->r10 ? (uint32_t)f->r10
                                         : g_current_proc->pid);
    f->rdi = tok.lower;
    return (int64_t)tok.upper;
}

static __attribute__((unused)) int64_t sys_cap_delegate_h(syscall_frame_t *f)
{
    if (!g_current_proc) return -1;
    cap_token_t src = { f->rdi, f->rsi };
    cap_token_t d   = cap_delegate(src, g_current_proc->pid,
                                   (uint32_t)f->rdx, f->r10);
    if (!d.upper && !d.lower) return -1;
    f->rdi = d.lower;
    return (int64_t)d.upper;
}

static __attribute__((unused)) int64_t sys_cap_revoke_h(syscall_frame_t *f)
{
    if (!g_current_proc) return -1;
    cap_token_t authority = { f->rdi, f->rsi };
    cap_token_t target    = { f->rdx, f->r10 };
    return cap_revoke(authority, target, g_current_proc->pid) ? 0 : -1;
}

static __attribute__((unused)) int64_t sys_set_intent(syscall_frame_t *f)
{
    proc_intent_t intent = (proc_intent_t)f->rdi;
    if (intent >= INTENT_COUNT || !g_current_proc) return -1;
    if (intent == INTENT_AUDIT) {
        cap_token_t tok = { f->rsi, f->rdx };
        if (!broker_check(g_current_proc->pid, tok, CAP_RIGHT_AUDIT))
            return -1;
    }
    sched_dequeue(g_current_proc);
    g_current_proc->intent = intent;
    sched_enqueue(g_current_proc);
    return 0;
}

static __attribute__((unused)) int64_t sys_audit_read_h(syscall_frame_t *f)
{
    if (!g_current_proc) return -1;
    cap_token_t tok = { f->rdi, f->rsi };
    if (!broker_check(g_current_proc->pid, tok, CAP_RIGHT_AUDIT))
        return -1;
    return (int64_t)audit_total();
}


/*  Network syscalls  */


static bool net_cap_check(syscall_frame_t *f, uint64_t required_rights)
{
    if (!g_current_proc) return false;
    cap_token_t tok = { f->rdi, f->rsi };
    return broker_check(g_current_proc->pid, tok, required_rights);
}

static __attribute__((unused)) int64_t sys_socket(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_SEND | CAP_RIGHT_NET_RECV))
        return -1;
    sock_type_t type = (sock_type_t)f->rdx;
    return (int64_t)net_socket(type);
}

static __attribute__((unused)) int64_t sys_bind(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_BIND)) return -1;
    int      sock_fd = (int)(int64_t)f->rdx;
    uint16_t port    = (uint16_t)f->r10;
    return (int64_t)net_bind(sock_fd, port);
}

static __attribute__((unused)) int64_t sys_connect(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_SEND)) return -1;
    int      sock_fd = (int)(int64_t)f->rdx;
    ip4_t    ip      = (ip4_t)f->r10;
    uint16_t port    = (uint16_t)f->r8;
    return (int64_t)net_connect(sock_fd, ip, port);
}

static __attribute__((unused)) int64_t sys_listen(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_BIND)) return -1;
    int sock_fd = (int)(int64_t)f->rdx;
    return (int64_t)net_listen(sock_fd);
}

static __attribute__((unused)) int64_t sys_accept(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_RECV)) return -1;
    int sock_fd = (int)(int64_t)f->rdx;
    return (int64_t)net_accept(sock_fd);
}

static __attribute__((unused)) int64_t sys_net_send(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_SEND)) return -1;
    int         sock_fd = (int)(int64_t)f->rdx;
    const void *buf     = (const void *)(uintptr_t)f->r10;
    uint16_t    len     = (uint16_t)f->r8;
    if (!uptr_ok(f->r10, len)) return -1;
    return (int64_t)net_send(sock_fd, buf, len);
}

static __attribute__((unused)) int64_t sys_net_recv(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_RECV)) return -1;
    int      sock_fd = (int)(int64_t)f->rdx;
    void    *buf     = (void *)(uintptr_t)f->r10;
    uint16_t len     = (uint16_t)f->r8;
    if (!uptr_ok(f->r10, len)) return -1;
    return (int64_t)net_recv(sock_fd, buf, len);
}

static __attribute__((unused)) int64_t sys_net_close(syscall_frame_t *f)
{
    int sock_fd = (int)(int64_t)f->rdx;
    net_socket_close(sock_fd);
    return 0;
}

int icmp_send_echo(uint32_t dst, uint16_t seq);

static __attribute__((unused)) int64_t sys_ping(syscall_frame_t *f)
{
    uint32_t ip  = (uint32_t)f->rdi;
    serial_print("[PING] sys_ping called ip=");
    serial_printhex((uint64_t)ip);
    serial_print("\n");
    static uint16_t seq = 0;
    /* enable interrupts so NIC can receive during poll */
    __asm__ volatile("sti");
    int64_t r = (int64_t)icmp_send_echo(ip, seq++);
    __asm__ volatile("cli");
    return r;
}

static __attribute__((unused)) int64_t sys_getifaddr(syscall_frame_t *f)
{
    (void)f;
    netif_t *iface = netif_default();
    if (!iface) return -1;
    return (int64_t)iface->ip;
}


/*  Dispatch table  */


typedef int64_t (*syscall_fn_t)(syscall_frame_t *);



static __attribute__((unused)) int64_t sys_fb_pixel(syscall_frame_t *f)
{
    fb_put_pixel((uint32_t)f->rdi, (uint32_t)f->rsi, (uint32_t)f->rdx);
    return 0;
}
static __attribute__((unused)) int64_t sys_fb_rect(syscall_frame_t *f)
{
    fb_fill_rect((uint32_t)f->rdi, (uint32_t)f->rsi,
                 (uint32_t)f->rdx,  (uint32_t)f->r10, (uint32_t)f->r8);
    return 0;
}
static __attribute__((unused)) int64_t sys_fb_clear(syscall_frame_t *f)
{
    fb_clear((uint32_t)f->rdi);
    return 0;
}
static __attribute__((unused)) int64_t sys_fb_info(syscall_frame_t *f)
{
    uint32_t *out = (uint32_t *)(uintptr_t)f->rdi;
    if (!uptr_ok(f->rdi, 16)) return -1;
    out[0] = g_fb.width;
    out[1] = g_fb.height;
    out[2] = g_fb.active ? 1 : 0;
    out[3] = g_fb.bpp;
    return 0;
}



__attribute__((unused))
static __attribute__((unused)) int64_t sys_mouse_pos(syscall_frame_t *f)
{
    int32_t *out = (int32_t *)(uintptr_t)f->rdi;
    if (!uptr_ok(f->rdi, 12)) return -1;
    out[0] = g_mouse.x;
    out[1] = g_mouse.y;
    out[2] = (g_mouse.left ? 1 : 0) |
             (g_mouse.right ? 2 : 0) |
             (g_mouse.middle ? 4 : 0);
    return 0;
}

__attribute__((unused))
static __attribute__((unused)) int64_t sys_fb_str(syscall_frame_t *f)
{
    uint32_t x   = (uint32_t)f->rdi;
    uint32_t y   = (uint32_t)f->rsi;
    uint64_t ptr = f->rdx;
    uint32_t fg  = (uint32_t)f->r10;
    uint32_t bg  = (uint32_t)f->r8;
    if (!uptr_ok(ptr, 1)) return -1;
    font_draw_str(x, y, (const char *)(uintptr_t)ptr, fg, bg);
    return 0;
}



static __attribute__((unused)) int64_t sys_fb_circle(syscall_frame_t *f)
{
    fb_fill_circle((int)f->rdi, (int)f->rsi,
                   (int)f->rdx, (uint32_t)f->r10);
    return 0;
}

static __attribute__((unused)) int64_t sys_fb_rrect(syscall_frame_t *f)
{
    fb_fill_rounded_rect((int)f->rdi,  (int)f->rsi,
                         (int)f->rdx,  (int)f->r10,
                         (int)f->r8,   (uint32_t)f->r9);
    return 0;
}

static __attribute__((unused)) int64_t sys_fb_flip(syscall_frame_t *f)
{
    (void)f;
    fb_flip();
    return 0;
}



static __attribute__((unused)) int64_t sys_chdir(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    const char *path = (const char *)(uintptr_t)f->rdi;
    return (int64_t)vfs_chdir(path);
}

static __attribute__((unused)) int64_t sys_getcwd(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, f->rsi)) return -1;
    char *buf = (char *)(uintptr_t)f->rdi;
    uint64_t size = f->rsi;
    return (int64_t)vfs_getcwd(buf, size);
}

extern void jump_to_userspace(uint64_t entry, uint64_t stack_top, uint64_t pml4);

void proc_trampoline(void)
{
    process_t *p = g_current_proc;
    uint64_t entry = p->user_entry;
    uint64_t stack = p->user_stack_top;
    uint64_t cr3   = p->cr3;
    __asm__ volatile(
        "cli\n"
        "jmp jump_to_userspace\n"
        :
        : "D"(entry), "S"(stack), "d"(cr3)
    );
    __builtin_unreachable();
}

static __attribute__((unused)) int64_t sys_spawn(syscall_frame_t *f)
{

    /* 1. Copy path from userspace BEFORE switching CR3 */
    if (!uptr_ok(f->rdi, 1)) { serial_print("[SPAWN] uptr fail\n"); return -1; }
    const char *path = (const char *)(uintptr_t)f->rdi;
    char kpath[512];
    uint64_t plen = 0;
    while (plen < 511 && path[plen]) { kpath[plen] = path[plen]; plen++; }
    kpath[plen] = 0;
    if (plen == 0) return -1;

    /* 2. Switch to kernel PML4 */
    extern uint64_t vmm_get_kernel_pml4(void);
    uint64_t kpml4 = vmm_get_kernel_pml4();
    uint64_t saved_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(kpml4) : "memory");

    /* 3. Open ELF from disk */
    int fd = vfs_open(kpath, 0);
    if (fd < 0) {
        serial_print("[SPAWN] not found: ");
        serial_print(kpath);
        serial_print("\n");
        __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
        return -1;
    }

    /* 4. Read ELF into kernel buffer */
    uint8_t *buf = kmalloc(256 * 1024);
    if (!buf) {
        vfs_close(fd);
        __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
        return -1;
    }
    int64_t sz = vfs_read(fd, buf, 256 * 1024);
    vfs_close(fd);
    if (sz <= 0) {
        kfree(buf);
        __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
        return -1;
    }

    /* 5. Create child process */
    process_t *child = proc_create(INTENT_INTERACTIVE,
                                    g_current_proc ? g_current_proc->pid : 0);
    if (!child) {
        kfree(buf);
        __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
        return -1;
    }

    /* 6. Load ELF into child address space */
    uint64_t new_pml4 = 0, entry = 0, stk_top = 0;
    if (!elf_load(buf, (uint64_t)sz, &new_pml4, &entry, &stk_top)) {
        serial_print("[SPAWN] ELF load failed\n");
        kfree(buf);
        proc_exit(child->pid, -1);
        __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
        return -1;
    }
    kfree(buf);

    /* 7. Set up child context */
    child->cr3            = new_pml4;
    child->user_entry     = entry;
    child->user_stack_top = stk_top;
    child->context.r15    = 0;
    child->context.r14    = 0;
    child->context.r13    = 0;
    child->context.r12    = 0;
    child->context.rbx    = 0;
    child->context.rbp    = 0;
    child->context.rsp    = child->kernel_stack_top;
    child->context.rip    = (uint64_t)(uintptr_t)proc_trampoline;
    child->state          = PROC_READY;
    sched_enqueue(child);

    /* 8. Restore caller CR3 */
    __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");

    serial_print("[SPAWN] PID=");
    serial_printhex((uint64_t)child->pid);
    serial_print(" entry=");
    serial_printhex(entry);
    serial_print("\n");

    return (int64_t)child->pid;
}


static __attribute__((unused)) int64_t sys_readdir(syscall_frame_t *f)
{
    typedef struct { uint64_t inode; uint8_t type; char name[256]; } dirent_user_t;
    int      fd  = (int)(int64_t)f->rdi;
    uint64_t buf = f->rsi;
    uint64_t max = f->rdx;
    if (max > USER_ADDR_MAX / sizeof(dirent_user_t)) return -1;
    if (!uptr_ok(buf, max * sizeof(dirent_user_t))) return -1;
    return vfs_readdir(fd, (void *)(uintptr_t)buf, max);
}

static __attribute__((unused)) int64_t sys_create(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    const char *path = (const char *)(uintptr_t)f->rdi;
    uint8_t type = (uint8_t)f->rsi;
    return (int64_t)vfs_create(path, type);
}
extern uint64_t g_ticks;
static __attribute__((unused)) int64_t sys_uptime(syscall_frame_t *f)
{
    (void)f;
    return (int64_t)(g_ticks / 100); /* 100Hz PIT -> seconds */
}

static __attribute__((unused)) int64_t sys_poweroff(syscall_frame_t *f)
{
    (void)f;
    serial_print("[SYS] Shutting down...\n");

    __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));

    for(;;) __asm__ volatile ("cli; hlt");
    return 0;
}

static __attribute__((unused)) int64_t sys_reboot(syscall_frame_t *f)
{
    (void)f;
    serial_print("[SYS] Rebooting...\n");

    uint32_t t = 100000;
    while (t--) { __asm__ volatile("" ::: "memory"); }

    /* Method 1: PS/2 keyboard controller reset */
    __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));

    t = 100000;
    while (t--) { __asm__ volatile("" ::: "memory"); }

    /* Method 2: triple fault via null IDT */
    __asm__ volatile (
        "lidt %0\n"
        "int $0\n"
        :: "m"(*(uint8_t*)0)
    );

    for(;;) __asm__ volatile ("cli; hlt");
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
    [SYS_POWEROFF]    = sys_poweroff,
    [SYS_REBOOT]      = sys_reboot,
    [SYS_SPAWN]       = sys_spawn,
    [SYS_UPTIME]      = sys_uptime,
    [SYS_MOUSE_POS]   = sys_mouse_pos,
    [SYS_FB_STR]      = sys_fb_str,
    [SYS_FB_PIXEL]    = sys_fb_pixel,
    [SYS_FB_RECT]     = sys_fb_rect,
    [SYS_FB_CLEAR]    = sys_fb_clear,
    [SYS_FB_INFO]     = sys_fb_info,
    [SYS_READDIR]     = sys_readdir,
    [SYS_CREATE]      = sys_create,
    [SYS_SOCKET]       = sys_socket,
    [SYS_BIND]         = sys_bind,
    [SYS_CONNECT]      = sys_connect,
    [SYS_LISTEN]       = sys_listen,
    [SYS_ACCEPT]       = sys_accept,
    [SYS_NET_SEND]     = sys_net_send,
    [SYS_NET_RECV]     = sys_net_recv,
    [SYS_NET_CLOSE]    = sys_net_close,
    [SYS_GETIFADDR]    = sys_getifaddr,
    [SYS_PING]        = sys_ping,
    [SYS_FB_CIRCLE]   = sys_fb_circle,
    [SYS_FB_RRECT]    = sys_fb_rrect,
    [SYS_FB_FLIP]     = sys_fb_flip,
    [SYS_CHDIR]       = sys_chdir,
    [SYS_GETCWD]      = sys_getcwd,
};

void syscall_dispatch(syscall_frame_t *frame)
{
    uint64_t num = frame->rax;

    audit_record(AUDIT_SYSCALL,
                 g_current_proc ? g_current_proc->pid : 0,
                 num, frame->rdi);

    if (num >= SYS_COUNT || !g_syscall_table[num]) {
        serial_print("[SYS] unhandled ");
        serial_printhex(num);
        serial_print("\n");
        frame->rax = (uint64_t)-1LL;
        return;
    }

    frame->rax = (uint64_t)g_syscall_table[num](frame);
}






/* forward declaration */
