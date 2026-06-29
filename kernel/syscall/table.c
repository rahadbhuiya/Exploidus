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
#include "../cnsl/cnsl.h"
#include "../fs/vfs/vfs.h"
#include "../mm/kmalloc.h"
#include "../elf/elf.h"
#include "../ipc/ipc.h"
#include "../drivers/fb_console.h"
#include "../shm/shm.h"
extern int64_t vfs_readdir(int fd, void *buf, uint64_t max);
extern int vfs_create(const char *path, uint8_t type);
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../crypto/blake3.h"
#include "../net/dns/dns.h"
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

static bool uptr_ok(uint64_t addr, uint64_t len)
{
    if (!addr) return false;
    if (addr > USER_ADDR_MAX) return false;
    if (!len)  return true;
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

    if (fd < 0 && (flags & O_CREAT)) {
        int created_fd = vfs_create(path, 0);
        if (created_fd >= 0) vfs_close(created_fd);
        fd = vfs_open(path, flags);
    }

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
    uint64_t ticks = f->rdi;
    if (ticks == 0) return 0;
    if (g_current_proc) {
        g_current_proc->wake_tick = g_uptime_ticks + ticks;
        g_current_proc->state     = PROC_BLOCKED;
        sched_yield();
    }
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
        if (!vmm_map_into(g_current_proc->cr3, phys,
                          virt + pg * MMAP_PAGE_SIZE,
                          VMM_WRITE | VMM_USER | VMM_NX)) {
            serial_print("[MMAP] vmm_map_into failed at pg="); serial_printhex(pg); serial_print("\n");
            pmm_free(phys);
            return -1;
        }
    }
    g_current_proc->mmap_top = virt;
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
        audit_record(AUDIT_CAP_DENIED, g_current_proc ? g_current_proc->pid : 0, 0, 0);
        return -1;
    }
    cap_token_t tok = cap_create((cap_resource_t)f->rdi,
                                  (uint32_t)f->rsi, f->rdx,
                                  f->r10 ? (uint32_t)f->r10 : g_current_proc->pid);
    f->rdi = tok.lower;
    return (int64_t)tok.upper;
}

static __attribute__((unused)) int64_t sys_cap_delegate_h(syscall_frame_t *f)
{
    if (!g_current_proc) return -1;
    cap_token_t src = { f->rdi, f->rsi };
    cap_token_t d   = cap_delegate(src, g_current_proc->pid, (uint32_t)f->rdx, f->r10);
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
        if (!broker_check(g_current_proc->pid, tok, CAP_RIGHT_AUDIT)) return -1;
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
    if (!broker_check(g_current_proc->pid, tok, CAP_RIGHT_AUDIT)) return -1;
    return (int64_t)audit_total();
}


/*  Network syscalls  */

static bool net_cap_check(syscall_frame_t *f, uint64_t required_rights)
{
    if (!g_current_proc) return false;
    cap_token_t tok = { f->rdi, f->rsi };
    if (tok.upper == 0 && tok.lower == 0) return true;
    return broker_check(g_current_proc->pid, tok, required_rights);
}

static __attribute__((unused)) int64_t sys_socket(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_SEND | CAP_RIGHT_NET_RECV)) return -1;
    sock_type_t type = (sock_type_t)f->rdx;
    return (int64_t)net_socket(type);
}
static __attribute__((unused)) int64_t sys_bind(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_BIND)) return -1;
    int sock_fd = (int)(int64_t)f->rdx; uint16_t port = (uint16_t)f->r10;
    return (int64_t)net_bind(sock_fd, port);
}
static __attribute__((unused)) int64_t sys_connect(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_SEND)) return -1;
    int sock_fd = (int)(int64_t)f->rdx; ip4_t ip = (ip4_t)f->r10; uint16_t port = (uint16_t)f->r8;
    return (int64_t)net_connect(sock_fd, ip, port);
}
static __attribute__((unused)) int64_t sys_listen(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_BIND)) return -1;
    return (int64_t)net_listen((int)(int64_t)f->rdx);
}
static __attribute__((unused)) int64_t sys_accept(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_RECV)) return -1;
    return (int64_t)net_accept((int)(int64_t)f->rdx);
}
static __attribute__((unused)) int64_t sys_net_send(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_SEND)) return -1;
    int sock_fd = (int)(int64_t)f->rdx; const void *buf = (const void *)(uintptr_t)f->r10; uint16_t len = (uint16_t)f->r8;
    if (!uptr_ok(f->r10, len)) return -1;
    return (int64_t)net_send(sock_fd, buf, len);
}
static __attribute__((unused)) int64_t sys_net_recv(syscall_frame_t *f)
{
    if (!net_cap_check(f, CAP_RIGHT_NET_RECV)) return -1;
    int sock_fd = (int)(int64_t)f->rdx; void *buf = (void *)(uintptr_t)f->r10; uint16_t len = (uint16_t)f->r8;
    if (!uptr_ok(f->r10, len)) return -1;
    return (int64_t)net_recv(sock_fd, buf, len);
}
static __attribute__((unused)) int64_t sys_net_close(syscall_frame_t *f)
{
    net_socket_close((int)(int64_t)f->rdx); return 0;
}

int icmp_send_echo(uint32_t dst, uint16_t seq);
static __attribute__((unused)) int64_t sys_ping(syscall_frame_t *f)
{
    uint32_t ip = (uint32_t)f->rdi;
    serial_print("[PING] sys_ping called ip="); serial_printhex((uint64_t)ip); serial_print("\n");
    static uint16_t seq = 0;
    __asm__ volatile("sti");
    int64_t r = (int64_t)icmp_send_echo(ip, seq++);
    __asm__ volatile("cli");
    return r;
}
static __attribute__((unused)) int64_t sys_getifaddr(syscall_frame_t *f)
{
    (void)f; netif_t *iface = netif_default(); if (!iface) return -1; return (int64_t)iface->ip;
}


/*  Dispatch table typedef  */

typedef int64_t (*syscall_fn_t)(syscall_frame_t *);


/*  Framebuffer syscalls  */

static __attribute__((unused)) int64_t sys_fb_pixel(syscall_frame_t *f)
{ fb_put_pixel((uint32_t)f->rdi, (uint32_t)f->rsi, (uint32_t)f->rdx); return 0; }
static __attribute__((unused)) int64_t sys_fb_rect(syscall_frame_t *f)
{ fb_fill_rect((uint32_t)f->rdi,(uint32_t)f->rsi,(uint32_t)f->rdx,(uint32_t)f->r10,(uint32_t)f->r8); return 0; }
static __attribute__((unused)) int64_t sys_fb_clear(syscall_frame_t *f)
{ fb_clear((uint32_t)f->rdi); return 0; }
static __attribute__((unused)) int64_t sys_fb_info(syscall_frame_t *f)
{
    uint32_t *out = (uint32_t *)(uintptr_t)f->rdi;
    if (!uptr_ok(f->rdi, 16)) return -1;
    out[0] = g_fb.width; out[1] = g_fb.height; out[2] = g_fb.active ? 1 : 0; out[3] = g_fb.bpp;
    return 0;
}
__attribute__((unused))
static __attribute__((unused)) int64_t sys_mouse_pos(syscall_frame_t *f)
{
    int32_t *out = (int32_t *)(uintptr_t)f->rdi;
    if (!uptr_ok(f->rdi, 12)) return -1;
    out[0] = g_mouse.x; out[1] = g_mouse.y;
    out[2] = (g_mouse.left ? 1 : 0) | (g_mouse.right ? 2 : 0) | (g_mouse.middle ? 4 : 0);
    return 0;
}
__attribute__((unused))
static __attribute__((unused)) int64_t sys_fb_str(syscall_frame_t *f)
{
    uint32_t x = (uint32_t)f->rdi; uint32_t y = (uint32_t)f->rsi;
    uint64_t ptr = f->rdx; uint32_t fg = (uint32_t)f->r10; uint32_t bg = (uint32_t)f->r8;
    if (!uptr_ok(ptr, 1)) return -1;
    font_draw_str(x, y, (const char *)(uintptr_t)ptr, fg, bg);
    return 0;
}
static __attribute__((unused)) int64_t sys_fb_circle(syscall_frame_t *f)
{ fb_fill_circle((int)f->rdi,(int)f->rsi,(int)f->rdx,(uint32_t)f->r10); return 0; }
static __attribute__((unused)) int64_t sys_fb_rrect(syscall_frame_t *f)
{ fb_fill_rounded_rect((int)f->rdi,(int)f->rsi,(int)f->rdx,(int)f->r10,(int)f->r8,(uint32_t)f->r9); return 0; }
static __attribute__((unused)) int64_t sys_fb_flip(syscall_frame_t *f)
{ (void)f; fb_flip(); return 0; }


/*  Filesystem/process misc  */

static __attribute__((unused)) int64_t sys_chdir(syscall_frame_t *f)
{ if (!uptr_ok(f->rdi,1)) return -1; return (int64_t)vfs_chdir((const char *)(uintptr_t)f->rdi); }
static __attribute__((unused)) int64_t sys_getcwd(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi,f->rsi)) return -1;
    return (int64_t)vfs_getcwd((char *)(uintptr_t)f->rdi, f->rsi);
}

extern void jump_to_userspace(uint64_t entry, uint64_t stack_top, uint64_t pml4);

void proc_trampoline(void)
{
    process_t *p = g_current_proc;
    uint64_t entry = p->user_entry; uint64_t stack = p->user_stack_top; uint64_t cr3 = p->cr3;
    __asm__ volatile("cli\njmp jump_to_userspace\n" : : "D"(entry),"S"(stack),"d"(cr3));
    __builtin_unreachable();
}

static int64_t sys_execv(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    const char *upath = (const char *)(uintptr_t)f->rdi;
    static char kpath[512];
    int plen = 0;
    while (plen < 511 && upath[plen]) { kpath[plen] = upath[plen]; plen++; }
    kpath[plen] = 0;
    if (plen == 0) return -1;

    static char   argv_store[4096];
    static const char *kargv[32];
    int argc = 0, boff = 0;
    if (f->rsi && uptr_ok(f->rsi, 8)) {
        uint64_t *uargv = (uint64_t *)(uintptr_t)f->rsi;
        while (argc < 31 && uargv[argc]) {
            const char *uarg = (const char *)(uintptr_t)uargv[argc];
            if (!uptr_ok((uint64_t)(uintptr_t)uarg, 1)) break;
            kargv[argc] = argv_store + boff;
            int l = 0;
            while (boff < 4094 && uarg[l]) argv_store[boff++] = uarg[l++];
            argv_store[boff++] = 0;
            argc++;
        }
    }
    kargv[argc] = (const char *)0;
    if (argc == 0) { kargv[0] = kpath; kargv[1] = (const char *)0; }

    extern uint64_t vmm_get_kernel_pml4(void);
    uint64_t kpml4 = vmm_get_kernel_pml4();
    uint64_t saved_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(kpml4) : "memory");

    int fd = vfs_open(kpath, 0);
    if (fd < 0) { __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory"); return -2; }
    uint8_t *buf = kmalloc(640 * 1024);
    if (!buf) { vfs_close(fd); __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory"); return -1; }
    int64_t sz = vfs_read(fd, buf, 640 * 1024);
    vfs_close(fd);
    if (sz <= 0) { kfree(buf); __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory"); return -1; }

    process_t *child = proc_create(INTENT_INTERACTIVE, g_current_proc ? g_current_proc->pid : 0);
    if (!child) { kfree(buf); __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory"); return -1; }

    uint64_t new_pml4 = 0, entry = 0, stk_top = 0;
    if (!elf_load(buf, (uint64_t)sz, &new_pml4, &entry, &stk_top, kargv, (const char **)0)) {
        kfree(buf); proc_exit(child->pid, -1);
        __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory"); return -1;
    }
    kfree(buf);
    child->cr3 = new_pml4; child->user_entry = entry; child->user_stack_top = stk_top;
    child->state = PROC_READY;
    __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
    return (int64_t)child->pid;
}

static __attribute__((unused)) int64_t sys_spawn(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) { serial_print("[SPAWN] uptr fail\n"); return -1; }
    const char *path = (const char *)(uintptr_t)f->rdi;
    static char kpath[512]; uint64_t plen = 0;
    while (plen < 511 && path[plen]) { kpath[plen] = path[plen]; plen++; }
    kpath[plen] = 0;
    if (plen == 0) return -1;

    static char kargs[512]; kargs[0] = 0;
    if (f->rdx && uptr_ok(f->rdx, 1)) {
        const char *uargs = (const char *)(uintptr_t)f->rdx; uint64_t alen = 0;
        while (alen < 511 && uargs[alen]) { kargs[alen] = uargs[alen]; alen++; }
        kargs[alen] = 0;
    }

    extern uint64_t vmm_get_kernel_pml4(void);
    uint64_t kpml4 = vmm_get_kernel_pml4(); uint64_t saved_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(saved_cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(kpml4) : "memory");

    int fd = vfs_open(kpath, 0);
    if (fd < 0) {
        serial_print("[SPAWN] not found: "); serial_print(kpath); serial_print("\n");
        __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory"); return -1;
    }
    uint8_t *buf = kmalloc(640 * 1024);
    if (!buf) { vfs_close(fd); __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory"); return -1; }
    int64_t sz = vfs_read(fd, buf, 640 * 1024);
    vfs_close(fd);
    serial_print("[SPAWN] vfs_read sz="); serial_printhex((uint64_t)(int64_t)sz); serial_print("\n");
    if (sz <= 0) { kfree(buf); __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory"); return -1; }

    proc_intent_t spawn_intent = (proc_intent_t)f->rsi;
    if (spawn_intent >= INTENT_COUNT) spawn_intent = INTENT_INTERACTIVE;
    process_t *child = proc_create(spawn_intent, g_current_proc ? g_current_proc->pid : 0);
    if (!child) { kfree(buf); __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory"); return -1; }

    uint64_t new_pml4 = 0, entry = 0, stk_top = 0;
    const char *spawn_argv[3];
    spawn_argv[0] = kpath;
    if (kargs[0]) { spawn_argv[1] = kargs; spawn_argv[2] = (const char *)0; }
    else          { spawn_argv[1] = (const char *)0; }

    if (!elf_load(buf, (uint64_t)sz, &new_pml4, &entry, &stk_top, spawn_argv, (const char **)0)) {
        serial_print("[SPAWN] ELF load failed\n");
        kfree(buf); proc_exit(child->pid, -1);
        __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory"); return -1;
    }
    kfree(buf);
    child->cr3 = new_pml4; child->user_entry = entry; child->user_stack_top = stk_top;
    child->context.r15 = 0; child->context.r14 = 0; child->context.r13 = 0;
    child->context.r12 = 0; child->context.rbx = 0; child->context.rbp = 0;
    child->context.rsp = child->kernel_stack_top;
    child->context.rip = (uint64_t)(uintptr_t)proc_trampoline;
    child->state = PROC_READY;
    sched_enqueue(child);
    __asm__ volatile("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
    serial_print("[SPAWN] PID="); serial_printhex((uint64_t)child->pid); serial_print("\n");
    return (int64_t)child->pid;
}

static __attribute__((unused)) int64_t sys_readdir(syscall_frame_t *f)
{
    typedef struct { uint64_t inode; uint8_t type; char name[256]; } dirent_user_t;
    int fd = (int)(int64_t)f->rdi; uint64_t buf = f->rsi; uint64_t max = f->rdx;
    if (max > USER_ADDR_MAX / sizeof(dirent_user_t)) return -1;
    if (!uptr_ok(buf, max * sizeof(dirent_user_t))) return -1;
    return vfs_readdir(fd, (void *)(uintptr_t)buf, max);
}
static __attribute__((unused)) int64_t sys_create(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    return (int64_t)vfs_create((const char *)(uintptr_t)f->rdi, (uint8_t)f->rsi);
}
extern uint64_t g_ticks;
static __attribute__((unused)) int64_t sys_uptime(syscall_frame_t *f)
{ (void)f; return (int64_t)(g_ticks / 100); }
static __attribute__((unused)) int64_t sys_poweroff(syscall_frame_t *f)
{
    (void)f; serial_print("[SYS] Shutting down...\n");
    __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));
    __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x4004));
    for(;;) { __asm__ volatile ("cli; hlt"); }
    return 0;
}
static __attribute__((unused)) int64_t sys_reboot(syscall_frame_t *f)
{
    (void)f; serial_print("[SYS] Rebooting...\n"); __asm__ volatile ("cli");
    uint32_t t = 100000; while (t--) __asm__ volatile ("" ::: "memory");
    __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    t = 500000; while (t--) __asm__ volatile ("" ::: "memory");
    static const struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile ("lidt %0\n int $0\n" :: "m"(null_idt));
    for(;;) { __asm__ volatile ("cli; hlt"); }
    return 0;
}
static __attribute__((unused)) int64_t sys_cnsl_unblock(syscall_frame_t *f)
{ bool ok = cnsl_unblock((uint32_t)f->rdi); return ok ? 0 : -1; }
static __attribute__((unused)) int64_t sys_cnsl_block_ttl(syscall_frame_t *f)
{ return (int64_t)cnsl_get_block_ttl((uint32_t)f->rdi); }
static __attribute__((unused)) int64_t sys_cnsl_list(syscall_frame_t *f)
{
    uint64_t max = f->rsi; if (max == 0) return 0;
    if (max > USER_ADDR_MAX / sizeof(cnsl_list_entry_t)) return -1;
    uint64_t bytes = max * sizeof(cnsl_list_entry_t);
    if (!uptr_ok(f->rdi, bytes)) return -1;
    return (int64_t)cnsl_list((cnsl_list_entry_t *)(uintptr_t)f->rdi, (uint16_t)max);
}
static __attribute__((unused)) int64_t sys_lseek(syscall_frame_t *f)
{ return vfs_lseek((int)(int64_t)f->rdi,(int64_t)f->rsi,(int)(int64_t)f->rdx); }
static __attribute__((unused)) int64_t sys_stat(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi,1)) return -1;
    if (!uptr_ok(f->rsi,sizeof(vfs_stat_t))) return -1;
    return (int64_t)vfs_stat((const char *)(uintptr_t)f->rdi,(vfs_stat_t *)(uintptr_t)f->rsi);
}
static __attribute__((unused)) int64_t sys_fstat(syscall_frame_t *f)
{
    if (!uptr_ok(f->rsi,sizeof(vfs_stat_t))) return -1;
    return (int64_t)vfs_fstat((int)(int64_t)f->rdi,(vfs_stat_t *)(uintptr_t)f->rsi);
}
static __attribute__((unused)) int64_t sys_dup(syscall_frame_t *f)
{ return (int64_t)vfs_dup((int)(int64_t)f->rdi); }
static __attribute__((unused)) int64_t sys_dup2(syscall_frame_t *f)
{ return (int64_t)vfs_dup2((int)(int64_t)f->rdi,(int)(int64_t)f->rsi); }


/*  HTTP / file / blake3  */

static __attribute__((unused)) int64_t sys_http_get(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1))      return -1;
    if (!uptr_ok(f->rsi, f->rdx)) return -1;
    const char *url = (const char *)(uintptr_t)f->rdi;
    uint8_t *buf = (uint8_t *)(uintptr_t)f->rsi;
    uint64_t bsz = f->rdx;
    if (url[0]!='h'||url[1]!='t'||url[2]!='t'||url[3]!='p') return -2;
    const char *host_start = url + 7;
    const char *slash = host_start;
    while (*slash && *slash != '/' && *slash != ':') slash++;
    if (!*slash) return -3;
    char host[64]; uint32_t hlen = (uint32_t)(slash - host_start);
    if (hlen >= 64) return -4;
    for (uint32_t i = 0; i < hlen; i++) host[i] = host_start[i];
    host[hlen] = 0;
    uint16_t port = 80; const char *path = slash;
    if (*slash == ':') { slash++; port = 0; while (*slash>='0'&&*slash<='9') port=port*10+(*slash++-'0'); path=slash; }
    if (!*path) path = "/";
    ip4_t ip = 0; const char *p = host; int dots = 0;
    for (const char *t=host;*t;t++) if(*t=='.') dots++;
    if (dots==3) {
        bool all_digits=true; for(const char *t=host;*t;t++) if(*t!='.'&&(*t<'0'||*t>'9')){all_digits=false;break;}
        if (all_digits) { for(int i=0;i<4;i++){uint8_t oct=0;while(*p>='0'&&*p<='9')oct=(uint8_t)(oct*10+(*p++-'0'));ip=(ip<<8)|oct;if(*p=='.')p++;} }
    }
    if (!ip) { ip=dns_resolve(host); if(!ip) return -5; }
    int sock=(int)net_socket(SOCK_TCP); if(sock<0) return -6;
    if(net_connect(sock,ip,port)<0){net_socket_close(sock);return -7;}
    char req[512]; uint32_t rlen=0;
    const char *method="GET "; for(const char *s=method;*s;s++) req[rlen++]=*s;
    for(const char *s=path;*s;s++) req[rlen++]=*s;
    const char *ver=" HTTP/1.0\r\nHost: "; for(const char *s=ver;*s;s++) req[rlen++]=*s;
    for(const char *s=host;*s;s++) req[rlen++]=*s;
    const char *tail="\r\nConnection: close\r\n\r\n"; for(const char *s=tail;*s;s++) req[rlen++]=*s;
    net_send(sock,req,(uint16_t)rlen);
    static uint8_t rbuf[4096]; int64_t total=0; int headers_done=0; uint8_t hdr_buf[4]={0,0,0,0}; uint32_t empty_recvs=0;
    char status_line[32]; uint32_t status_len=0; int status_done=0; int http_status=0;
    while(1){
        int64_t n=net_recv(sock,rbuf,sizeof(rbuf));
        if(n<=0){empty_recvs++;if(empty_recvs>500)break;continue;}
        empty_recvs=0;
        for(int64_t i=0;i<n;i++){
            uint8_t c=rbuf[i];
            if(!status_done){
                if(c=='\n'){status_line[status_len]=0;status_done=1;
                    const char *sp=status_line;while(*sp&&*sp!=' ')sp++;while(*sp==' ')sp++;
                    int code=0,digits=0;while(*sp>='0'&&*sp<='9'&&digits<3){code=code*10+(*sp-'0');sp++;digits++;}
                    http_status=code;if(http_status<200||http_status>=300){net_socket_close(sock);return -8;}
                }else if(c!='\r'&&status_len<sizeof(status_line)-1)status_line[status_len++]=(char)c;
            }
            if(!headers_done){hdr_buf[0]=hdr_buf[1];hdr_buf[1]=hdr_buf[2];hdr_buf[2]=hdr_buf[3];hdr_buf[3]=c;
                if(hdr_buf[0]=='\r'&&hdr_buf[1]=='\n'&&hdr_buf[2]=='\r'&&hdr_buf[3]=='\n')headers_done=1;
            }else{if((uint64_t)total<bsz)buf[total++]=c;}
        }
    }
    net_socket_close(sock); return total;
}

static __attribute__((unused)) int64_t sys_blake3(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, f->rsi)) return -1;
    if (!uptr_ok(f->rdx, 32))     return -1;
    blake3_hash((const uint8_t *)(uintptr_t)f->rdi, f->rsi, (uint8_t *)(uintptr_t)f->rdx);
    return 0;
}
static __attribute__((unused)) int64_t sys_unlink(syscall_frame_t *f)
{ if (!uptr_ok(f->rdi,1)) return -1; return vfs_unlink((const char *)(uintptr_t)f->rdi); }

static __attribute__((unused)) int64_t sys_http_download(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi,1)) return -1;
    if (!uptr_ok(f->rsi,1)) return -1;
    if (f->rdx && !uptr_ok(f->rdx,32)) return -1;
    const char *url=(const char *)(uintptr_t)f->rdi;
    const char *dst_path=(const char *)(uintptr_t)f->rsi;
    uint8_t *hash_out=(uint8_t *)(uintptr_t)f->rdx;
    if(url[0]!='h'||url[1]!='t'||url[2]!='t'||url[3]!='p') return -2;
    const char *host_start = url + 7;
    const char *slash = host_start;
    while (*slash && *slash != '/' && *slash != ':') slash++;
    if (!*slash) return -3;
    char host[64];
    uint32_t hlen = (uint32_t)(slash - host_start);
    if (hlen >= 64) return -4;
    for (uint32_t i = 0; i < hlen; i++) host[i] = host_start[i];
    host[hlen] = 0;
    uint16_t port = 80;
    const char *path = slash;
    if (*slash == ':') {
        slash++;
        port = 0;
        while (*slash >= '0' && *slash <= '9')
            port = (uint16_t)(port * 10 + (*slash++ - '0'));
        path = slash;
    }
    if (!*path) path = "/";
    ip4_t ip = 0;
    const char *pp = host;
    int dots = 0;
    for (const char *t = host; *t; t++) if (*t == '.') dots++;
    if (dots == 3) {
        bool all_digits = true;
        for (const char *t = host; *t; t++)
            if (*t != '.' && (*t < '0' || *t > '9')) { all_digits = false; break; }
        if(all_digits){for(int i=0;i<4;i++){uint8_t oct=0;while(*pp>='0'&&*pp<='9')oct=(uint8_t)(oct*10+(*pp++-'0'));ip=(ip<<8)|oct;if(*pp=='.')pp++;}}}
    if(!ip){ip=dns_resolve(host);if(!ip)return -5;}
    int fd=vfs_open(dst_path,1);
    if(fd<0){int cfd=vfs_create(dst_path,0);if(cfd<0)return -9;vfs_close(cfd);fd=vfs_open(dst_path,1);if(fd<0)return -9;}
    int sock=(int)net_socket(SOCK_TCP); if(sock<0){vfs_close(fd);return -6;}
    if(net_connect(sock,ip,port)<0){vfs_close(fd);net_socket_close(sock);return -7;}
    static char req[512]; uint32_t rlen=0;
    const char *method="GET "; for(const char *s=method;*s;)req[rlen++]=*s++;
    for(const char *s=path;*s;)req[rlen++]=*s++;
    const char *ver=" HTTP/1.0\r\nHost: "; for(const char *s=ver;*s;)req[rlen++]=*s++;
    for(const char *s=host;*s;)req[rlen++]=*s++;
    const char *tail="\r\nConnection: close\r\n\r\n"; for(const char *s=tail;*s;)req[rlen++]=*s++;
    net_send(sock,req,(uint16_t)rlen);
    static uint8_t rbuf[4096]; int64_t total=0; int headers_done=0; uint8_t hdr_buf[4]={0,0,0,0}; uint32_t empty_recvs=0;
    char status_line[32]; uint32_t status_len=0; int status_done=0; int http_status=0;
    static uint8_t wbuf[4096]; uint32_t wbuf_pos=0;
    blake3_ctx_t hash_ctx; if(hash_out)blake3_init(&hash_ctx);
    while(1){
        int64_t n=net_recv(sock,rbuf,sizeof(rbuf));
        if(n<=0){empty_recvs++;if(empty_recvs>500)break;continue;}
        empty_recvs=0;
        for(int64_t i=0;i<n;i++){
            uint8_t c=rbuf[i];
            if(!status_done){
                if(c=='\n'){status_line[status_len]=0;status_done=1;
                    const char *sp=status_line;while(*sp&&*sp!=' ')sp++;while(*sp==' ')sp++;
                    int code=0,digits=0;while(*sp>='0'&&*sp<='9'&&digits<3){code=code*10+(*sp-'0');sp++;digits++;}
                    http_status=code;if(http_status<200||http_status>=300){vfs_close(fd);net_socket_close(sock);return -8;}
                }else if(c!='\r'&&status_len<sizeof(status_line)-1)status_line[status_len++]=(char)c;
            }
            if(!headers_done){hdr_buf[0]=hdr_buf[1];hdr_buf[1]=hdr_buf[2];hdr_buf[2]=hdr_buf[3];hdr_buf[3]=c;
                if(hdr_buf[0]=='\r'&&hdr_buf[1]=='\n'&&hdr_buf[2]=='\r'&&hdr_buf[3]=='\n')headers_done=1;
            }else{wbuf[wbuf_pos++]=c;
                if(wbuf_pos==sizeof(wbuf)){if(hash_out)blake3_update(&hash_ctx,wbuf,wbuf_pos);vfs_write(fd,wbuf,wbuf_pos);total+=wbuf_pos;wbuf_pos=0;}
            }
        }
    }
    if(wbuf_pos>0){if(hash_out)blake3_update(&hash_ctx,wbuf,wbuf_pos);vfs_write(fd,wbuf,wbuf_pos);total+=wbuf_pos;}
    vfs_close(fd); net_socket_close(sock);
    if(hash_out)blake3_final(&hash_ctx,hash_out);
    return total;
}

static __attribute__((unused)) int64_t sys_file_write(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi,1)) return -1;
    if (!uptr_ok(f->rsi,f->rdx)) return -1;
    const char *path=(const char *)(uintptr_t)f->rdi;
    const void *data=(const void *)(uintptr_t)f->rsi; uint64_t size=f->rdx;
    int fd=vfs_open(path,1);
    if(fd<0){int cfd=vfs_create(path,0);if(cfd<0)return -2;vfs_close(cfd);fd=vfs_open(path,1);if(fd<0)return -3;}
    int64_t written=vfs_write(fd,data,size); vfs_close(fd); return written;
}


/*  GUI: IPC syscalls  */

static __attribute__((unused)) int64_t sys_ipc_send(syscall_frame_t *f)
{
    uint32_t dst_pid = (uint32_t)f->rdi;
    if (!uptr_ok(f->rsi, sizeof(ipc_msg_t))) return -1;
    ipc_msg_t *umsg = (ipc_msg_t *)(uintptr_t)f->rsi;
    ipc_msg_t kmsg;
    kmsg.from_pid = g_current_proc ? g_current_proc->pid : 0;
    kmsg.type     = umsg->type;
    kmsg.len      = umsg->len < IPC_MSG_PAYLOAD ? umsg->len : IPC_MSG_PAYLOAD;
    for (uint32_t i = 0; i < kmsg.len; i++) kmsg.data[i] = umsg->data[i];
    return (int64_t)ipc_send(dst_pid, &kmsg);
}

static __attribute__((unused)) int64_t sys_ipc_recv(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, sizeof(ipc_msg_t))) return -1;
    ipc_msg_t *out = (ipc_msg_t *)(uintptr_t)f->rdi;
    ipc_msg_t kmsg;
    int r = ipc_recv(&kmsg);
    if (r == 0) *out = kmsg;
    return (int64_t)r;
}

static __attribute__((unused)) int64_t sys_ipc_recv_nb(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, sizeof(ipc_msg_t))) return -1;
    ipc_msg_t *out = (ipc_msg_t *)(uintptr_t)f->rdi;
    ipc_msg_t kmsg;
    int r = ipc_recv_nb(&kmsg);
    if (r == 0) *out = kmsg;
    return (int64_t)r;
}


/*  GUI: SHM syscalls  */

static __attribute__((unused)) int64_t sys_shm_create(syscall_frame_t *f)
{
    if (!f->rdi) return 0;
    uint32_t pid = g_current_proc ? g_current_proc->pid : 0;
    return (int64_t)shm_create(pid, f->rdi);
}

static __attribute__((unused)) int64_t sys_shm_map(syscall_frame_t *f)
{
    uint32_t pid = g_current_proc ? g_current_proc->pid : 0;
    return (int64_t)shm_map((uint32_t)f->rdi, pid);
}

static __attribute__((unused)) int64_t sys_shm_unmap(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, f->rsi)) return -1;
    uint32_t pid = g_current_proc ? g_current_proc->pid : 0;
    shm_unmap(f->rdi, f->rsi, pid);
    return 0;
}

static __attribute__((unused)) int64_t sys_shm_destroy(syscall_frame_t *f)
{
    uint32_t pid = g_current_proc ? g_current_proc->pid : 0;
    return (int64_t)shm_destroy((uint32_t)f->rdi, pid);
}

/*
 * SYS_FB_CONSOLE_SET(68)
 *   rdi = 0 → disable fb_console (GUI mode, compositor owns screen)
 *   rdi = 1 → enable  fb_console (text mode, default)
 * Returns previous state (0 or 1).
 */
static __attribute__((unused)) int64_t sys_fb_console_set(syscall_frame_t *f)
{
    int prev = fb_console_enabled();
    if (f->rdi == 0)
        fb_console_disable();
    else
        fb_console_enable();
    return (int64_t)prev;
}


/*  Dispatch table  */

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
    [SYS_HTTP_DOWNLOAD]= sys_http_download,
    [SYS_EXECV]        = sys_execv,
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