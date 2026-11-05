#include "table.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"
#include "../proc/fork_exec.h"
#include "../cap/broker.h"
#include "../cap/capability.h"
#include "../audit/audit.h"
#include "../mm/kmalloc.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../elf/elf.h"
#include "../ipc/ipc.h"
#include "../shm/shm.h"
#include "../sync/futex.h"
#include "../crypto/blake3.h"
#include "../drivers/rtc.h"
#include "../drivers/serial.h"
#include <string.h>


static uint64_t sys_getrandom_rdrand64(void)
{
    uint64_t val = 0;
    uint8_t ok = 0;

    if (!sys_getrandom_has_rdrand()) {
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32 | lo) ^ 0xA5A5A5A5A5A5A5A5ULL;
    }

    for (int i = 0; i < 10; i++) {
        __asm__ volatile ("rdrand %0\n" "setc %1\n" : "=r"(val), "=qm"(ok));
        if (ok) break;
    }
    return val;
}



int64_t sys_exit(syscall_frame_t *f)
{
    int code = (int)(int64_t)f->rdi;
    if (g_current_proc) proc_exit(g_current_proc->pid, code);
    sched_yield();
    __asm__ volatile("cli");
    for(;;) __asm__ volatile("hlt");
    return 0;
}


int64_t sys_fork(syscall_frame_t *f)   { return sys_fork_impl(f); }


int64_t sys_exec(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, f->rsi)) return -1;
    return sys_exec_impl((const uint8_t *)(uintptr_t)f->rdi, f->rsi);
}


int64_t sys_waitpid(syscall_frame_t *f)
{
    return sys_waitpid_impl((uint32_t)f->rdi);
}



int64_t sys_execv(syscall_frame_t *f)
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



/*
 * sys_execve -- real execve(path, argv, envp): replaces the CALLING
 * process in place (see sys_execve_impl() in kernel/proc/fork_exec.c),
 * unlike sys_execv() above, which spawns a separate child process.
 * All user memory (path string, argv array + its strings, envp array
 */
int64_t sys_execve(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, 1)) return -1;
    const char *upath = (const char *)(uintptr_t)f->rdi;
    static char kpath[512];
    int plen = 0;
    while (plen < 511 && upath[plen]) { kpath[plen] = upath[plen]; plen++; }
    kpath[plen] = 0;
    if (plen == 0) return -1;

    static char        argv_store[4096];
    static const char *kargv[32];
    int argc = 0, aboff = 0;
    if (f->rsi && uptr_ok(f->rsi, 8)) {
        uint64_t *uargv = (uint64_t *)(uintptr_t)f->rsi;
        while (argc < 31 && uargv[argc]) {
            const char *uarg = (const char *)(uintptr_t)uargv[argc];
            if (!uptr_ok((uint64_t)(uintptr_t)uarg, 1)) break;
            kargv[argc] = argv_store + aboff;
            int l = 0;
            while (aboff < 4094 && uarg[l]) argv_store[aboff++] = uarg[l++];
            argv_store[aboff++] = 0;
            argc++;
        }
    }
    kargv[argc] = (const char *)0;
    /* Fall back to argv = [path] when the caller passed none, same
     * convention as sys_execv() and elf_load()'s own default. */
    if (argc == 0) { kargv[0] = kpath; kargv[1] = (const char *)0; }

    static char        envp_store[4096];
    static const char *kenvp[32];
    int envc = 0, eboff = 0;
    if (f->rdx && uptr_ok(f->rdx, 8)) {
        uint64_t *uenvp = (uint64_t *)(uintptr_t)f->rdx;
        while (envc < 31 && uenvp[envc]) {
            const char *uenv = (const char *)(uintptr_t)uenvp[envc];
            if (!uptr_ok((uint64_t)(uintptr_t)uenv, 1)) break;
            kenvp[envc] = envp_store + eboff;
            int l = 0;
            while (eboff < 4094 && uenv[l]) envp_store[eboff++] = uenv[l++];
            envp_store[eboff++] = 0;
            envc++;
        }
    }
    kenvp[envc] = (const char *)0;

    return sys_execve_impl(kpath, kargv, envc ? kenvp : (const char **)0);
}



int64_t sys_spawn(syscall_frame_t *f)
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


int64_t sys_getpid(syscall_frame_t *f) { (void)f; return g_current_proc ? (int64_t)g_current_proc->pid : -1; }


int64_t sys_yield(syscall_frame_t *f)  { (void)f; sched_yield(); return 0; }


int64_t sys_sleep(syscall_frame_t *f)
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



int64_t sys_getprocs(syscall_frame_t *f)
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



/*
 * sys_kill(pid, sig) -- send a signal to another (or the same)
 * process. This is a single-user system: any process may signal any
 * other live process, there is no owner/uid check here (a
 * multi-user build would check the caller's identity against the
 */
int64_t sys_kill(syscall_frame_t *f)
{
    int pid = (int)(int64_t)f->rdi;
    int sig = (int)(int64_t)f->rsi;

    if (sig <= 0 || sig >= 16) return -1;

    process_t *target = proc_get((uint32_t)pid);
    if (!target || target->state == PROC_ZOMBIE) return -1; /* no such process */

    target->pending_signals |= (1u << sig);
    return 0;
}



/*
 * sys_sigaction(signum, handler_addr) — registers a userspace signal
 * handler with the kernel, so idt.c's exception handler can redirect
 * execution to it on a crash instead of unconditionally killing the
 * process (see kernel/arch/x86_64/idt.c for the delivery side). A
 */
int64_t sys_sigaction(syscall_frame_t *f)
{
    int      signum  = (int)f->rdi;
    uint64_t handler = f->rsi;
    if (!g_current_proc) return -1;
    if (signum < 0 || signum >= 16) return -1;
    g_current_proc->sig_handlers[signum] = handler;
    return 0;
}



/*
 * sys_sigreturn() -- called by a userspace signal handler when it is
 * done and wants to resume the code the fault interrupted, instead of
 * calling exit(). Restores the full register state (including
 * rip/rsp/rflags) idt.c saved at fault time and jumps straight back
 */
int64_t sys_sigreturn(syscall_frame_t *f)
{
    (void)f;
    if (!g_current_proc || !g_current_proc->sig_in_progress) {
        return -1; /* not inside a handler, nothing to resume */
    }

    interrupt_frame_t restored = g_current_proc->sig_saved_frame;
    g_current_proc->sig_in_progress = false;

    sigreturn_restore(&restored);
    __builtin_unreachable();
}



/*
 * sys_set_tls(base) — sets the calling process's thread-local-storage
 * base (x86-64 FS_BASE). Native Exploidus syscall, not a Linux
 * arch_prctl clone — deliberately simpler (always sets FS_BASE, no
 * flag argument) since Exploidus doesn't need GS_BASE or the
 */
int64_t sys_set_tls(syscall_frame_t *f)
{
    uint64_t base = f->rdi;
    if (g_current_proc) g_current_proc->fs_base = base;

    uint32_t lo = (uint32_t)base;
    uint32_t hi = (uint32_t)(base >> 32);
    __asm__ volatile ("wrmsr" :: "c"(0xC0000100u), "a"(lo), "d"(hi));
    return 0;
}



/* sys_futex_wait(addr, expected) */
int64_t sys_futex_wait(syscall_frame_t *f)
{
    uint64_t addr     = f->rdi;
    uint32_t expected = (uint32_t)f->rsi;
    if (!uptr_ok(addr, 4)) return -1;
    futex_wait(addr, expected);
    return 0;
}



/* sys_futex_wake(addr, count) */
int64_t sys_futex_wake(syscall_frame_t *f)
{
    uint64_t addr  = f->rdi;
    uint32_t count = (uint32_t)f->rsi;
    if (!uptr_ok(addr, 4)) return -1;
    futex_wake(addr, count);
    return 0;
}



/*
 * sys_setuid(new_uid) -- voluntary, one-way privilege drop. Only
 * callable by a process currently at UID_ROOT, and only to switch to
 * a non-root uid; a process that has already dropped can never call
 * this again (whether to go back to root or to another non-root
 */
int64_t sys_setuid(syscall_frame_t *f)
{
    if (!g_current_proc) return -1;
    if (g_current_proc->uid != UID_ROOT) return -1; /* already dropped */

    uint32_t new_uid = (uint32_t)f->rdi;
    if (new_uid == UID_ROOT) return -1; /* this call only ever lowers */

    g_current_proc->uid = new_uid;
    return 0;
}



int64_t sys_getuid(syscall_frame_t *f)
{
    (void)f;
    return g_current_proc ? (int64_t)g_current_proc->uid : (int64_t)UID_ROOT;
}



/*
 * sys_setgid(new_gid) -- voluntary group assignment, mirroring
 * sys_setuid()'s rule exactly: only callable while the caller is
 * still at UID_ROOT (checked via uid, not gid -- so the correct
 * privilege-drop order is setgid() then setuid(), same order real
 */
int64_t sys_setgid(syscall_frame_t *f)
{
    if (!g_current_proc) return -1;
    if (g_current_proc->uid != UID_ROOT) return -1; /* must still be root */
    g_current_proc->gid = (uint32_t)f->rdi;
    return 0;
}



int64_t sys_getgid(syscall_frame_t *f)
{
    (void)f;
    return g_current_proc ? (int64_t)g_current_proc->gid : (int64_t)GID_ROOT;
}



/*
 * sys_meminfo -- fills a meminfo_t (kernel/mm/pmm.h) at rdi with real
 * physical-memory numbers. Backs the shell's `free` command, which
 * used to print a hardcoded "262144 / -- / --" placeholder
 * (cmd_ext_free() in userspace/shell/exploish_cmds.c) with no real
 */
int64_t sys_meminfo(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, sizeof(meminfo_t))) return -1;

    uint64_t total_pages = pmm_total_pages();
    uint64_t free_pages  = pmm_free_pages(ZONE_GREEN)
                          + pmm_free_pages(ZONE_YELLOW)
                          + pmm_free_pages(ZONE_RED);
    /* Guard against free > total (shouldn't happen, but this feeds a
     * subtraction below and used_kb wrapping to a huge number would
     * be a worse, more confusing failure mode than just clamping). */
    if (free_pages > total_pages) free_pages = total_pages;

    meminfo_t out;
    out.total_kb = (total_pages * PAGE_SIZE) / 1024;
    out.free_kb  = (free_pages  * PAGE_SIZE) / 1024;
    out.used_kb  = out.total_kb - out.free_kb;

    /* Build the whole struct locally, then copy out in one shot --
     * same reasoning as sys_execve()'s user-copy-then-switch pattern
     * elsewhere in this file: never write partially-computed state
     * straight into user memory. */
    *(meminfo_t *)(uintptr_t)f->rdi = out;
    return 0;
}



int64_t sys_getrandom(syscall_frame_t *f)
{
    uint64_t len = f->rsi;
    if (!uptr_ok(f->rdi, len)) return -1;

    uint8_t *out = (uint8_t *)(uintptr_t)f->rdi;
    uint64_t off = 0;

    while (off < len) {
        uint64_t r = sys_getrandom_rdrand64();
        uint64_t chunk = (len - off) < 8 ? (len - off) : 8;
        for (uint64_t i = 0; i < chunk; i++)
            out[off + i] = (uint8_t)(r >> (8 * i));
        off += chunk;
    }

    return 0;
}



int64_t sys_mmap(syscall_frame_t *f)
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



int64_t sys_munmap(syscall_frame_t *f)
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



int64_t sys_ipc_send(syscall_frame_t *f)
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



int64_t sys_ipc_recv(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, sizeof(ipc_msg_t))) return -1;
    ipc_msg_t *out = (ipc_msg_t *)(uintptr_t)f->rdi;
    ipc_msg_t kmsg;
    int r = ipc_recv(&kmsg);
    if (r == 0) *out = kmsg;
    return (int64_t)r;
}



int64_t sys_ipc_recv_nb(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, sizeof(ipc_msg_t))) return -1;
    ipc_msg_t *out = (ipc_msg_t *)(uintptr_t)f->rdi;
    ipc_msg_t kmsg;
    int r = ipc_recv_nb(&kmsg);
    if (r == 0) *out = kmsg;
    return (int64_t)r;
}



int64_t sys_shm_create(syscall_frame_t *f)
{
    if (!f->rdi) return 0;
    uint32_t pid = g_current_proc ? g_current_proc->pid : 0;
    return (int64_t)shm_create(pid, f->rdi);
}



int64_t sys_shm_map(syscall_frame_t *f)
{
    uint32_t pid = g_current_proc ? g_current_proc->pid : 0;
    return (int64_t)shm_map((uint32_t)f->rdi, pid);
}



int64_t sys_shm_unmap(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, f->rsi)) return -1;
    uint32_t pid = g_current_proc ? g_current_proc->pid : 0;
    shm_unmap(f->rdi, f->rsi, pid);
    return 0;
}



int64_t sys_shm_destroy(syscall_frame_t *f)
{
    uint32_t pid = g_current_proc ? g_current_proc->pid : 0;
    return (int64_t)shm_destroy((uint32_t)f->rdi, pid);
}



int64_t sys_cap_create_h(syscall_frame_t *f)
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



int64_t sys_cap_delegate_h(syscall_frame_t *f)
{
    if (!g_current_proc) return -1;
    cap_token_t src = { f->rdi, f->rsi };
    cap_token_t d   = cap_delegate(src, g_current_proc->pid, (uint32_t)f->rdx, f->r10);
    if (!d.upper && !d.lower) return -1;
    f->rdi = d.lower;
    return (int64_t)d.upper;
}



int64_t sys_cap_revoke_h(syscall_frame_t *f)
{
    if (!g_current_proc) return -1;
    cap_token_t authority = { f->rdi, f->rsi };
    cap_token_t target    = { f->rdx, f->r10 };
    return cap_revoke(authority, target, g_current_proc->pid) ? 0 : -1;
}



int64_t sys_set_intent(syscall_frame_t *f)
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



int64_t sys_audit_read_h(syscall_frame_t *f)
{
    if (!g_current_proc) return -1;
    cap_token_t tok = { f->rdi, f->rsi };
    if (!broker_check(g_current_proc->pid, tok, CAP_RIGHT_AUDIT)) return -1;
    return (int64_t)audit_total();
}



int64_t sys_audit_dump(syscall_frame_t *f)
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


int64_t sys_poweroff(syscall_frame_t *f)
{
    (void)f; serial_print("[SYS] Shutting down...\n");
    __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));
    __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x4004));
    for(;;) { __asm__ volatile ("cli; hlt"); }
    return 0;
}


int64_t sys_reboot(syscall_frame_t *f)
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


int64_t sys_uptime(syscall_frame_t *f)
{ (void)f; return (int64_t)(g_ticks / 100); }


int64_t sys_uptime_ticks(syscall_frame_t *f)
{ (void)f; return (int64_t)g_ticks; }



/*
 * sys_rtc_read() — returns the CMOS RTC date/time packed into a
 * single uint64: year(16) | month(8) | day(8) | hour(8) | min(8) | sec(8)
 * Packing avoids needing a user-pointer-out-parameter + uptr_ok dance
 * for six small fields.
 */
int64_t sys_rtc_read(syscall_frame_t *f)
{
    (void)f;
    rtc_time_t t;
    rtc_read(&t);
    uint64_t packed =
        ((uint64_t)t.year   << 48) |
        ((uint64_t)t.month  << 40) |
        ((uint64_t)t.day    << 32) |
        ((uint64_t)t.hour   << 24) |
        ((uint64_t)t.minute << 16) |
        ((uint64_t)t.second << 8);
    return (int64_t)packed;
}



int64_t sys_blake3(syscall_frame_t *f)
{
    if (!uptr_ok(f->rdi, f->rsi)) return -1;
    if (!uptr_ok(f->rdx, 32))     return -1;
    blake3_hash((const uint8_t *)(uintptr_t)f->rdi, f->rsi, (uint8_t *)(uintptr_t)f->rdx);
    return 0;
}


