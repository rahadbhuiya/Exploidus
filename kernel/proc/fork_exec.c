#include "fork_exec.h"
#include "process.h"
#include "scheduler.h"
#include "../elf/elf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../mm/kmalloc.h"
#include "../audit/audit.h"
#include "../drivers/serial.h"
#include "../arch/x86_64/idt.h"
#include <string.h>

#define PAGE_SIZE 4096UL


/*  Helper: walk and free an entire user-space address space            */
/*                                                                       */
/*  Only pages NOT present in the kernel PML4 are freed.               */
/*  Kernel half (entries 256-511) and any kernel-owned low entries      */
/*  are shared — never touched.                                         */


static void free_user_address_space(uint64_t pml4_phys)
{
    if (!pml4_phys) return;
    uint64_t *pml4      = (uint64_t *)(uintptr_t)pml4_phys;
    uint64_t *kpml4     = (uint64_t *)(uintptr_t)vmm_get_kernel_pml4();

    for (int p4 = 0; p4 < 256; p4++) {
        if (!(pml4[p4] & 1)) continue;
        /* Skip entries shared with the kernel PML4 */
        if (pml4[p4] == kpml4[p4]) continue;

        uint64_t pdpt_phys = pml4[p4] & VMM_ADDR_MASK;
        uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_phys;

        for (int p3 = 0; p3 < 512; p3++) {
            if (!(pdpt[p3] & 1)) continue;
            if (pdpt[p3] & VMM_HUGE) continue;
            uint64_t pd_phys = pdpt[p3] & VMM_ADDR_MASK;
            uint64_t *pd = (uint64_t *)(uintptr_t)pd_phys;

            for (int p2 = 0; p2 < 512; p2++) {
                if (!(pd[p2] & 1)) continue;
                if (pd[p2] & VMM_HUGE) continue;
                uint64_t pt_phys = pd[p2] & VMM_ADDR_MASK;
                uint64_t *pt = (uint64_t *)(uintptr_t)pt_phys;

                for (int p1 = 0; p1 < 512; p1++) {
                    if (!(pt[p1] & 1)) continue;
                    pmm_free(pt[p1] & VMM_ADDR_MASK);
                }
                pmm_free(pt_phys);
            }
            pmm_free(pd_phys);
        }
        pmm_free(pdpt_phys);
    }
    pmm_free(pml4_phys);
}


/*
 * fork_child_entry -- the first code a freshly-forked child runs.
 *
 * A process normally only ever enters userspace through
 * jump_to_userspace() (jump_userspace.asm), called directly right
 * after exec()/spawn() load an ELF. A forked child has no such call
 * site: it does not exec anything, it just needs to resume exactly
 * where the parent was, in the middle of the parent's own code. The
 * scheduler reaches this function like any other, by loading
 * context.rip and running it (context_switch.asm) -- so this is a
 * plain, ordinary function, but instead of returning it hands off to
 * sigreturn_restore() (kernel/arch/x86_64/sigreturn.asm) to restore
 * the full register state sys_fork_impl() saved into fork_frame at
 * fork time and iret straight into userspace. Never returns.
 */
void fork_child_entry(void)
{
    interrupt_frame_t frame = g_current_proc->fork_frame;
    sigreturn_restore(&frame);
    __builtin_unreachable();
}

/*  fork()     */


int64_t sys_fork_impl(syscall_frame_t *f)
{
    /*
     * Capture the callee-saved registers (rbx/rbp/r12-r15) as the
     * very first thing this function does, before any other C code
     * has a chance to use them as scratch space. entry.asm's syscall
     * prologue never touches these, and the C ABI guarantees any C
     * function called from here restores them before returning -- so
     * right now they still hold the exact values the parent's
     * userspace code had at the moment it called fork(). Everything
     * else needed for the child's resume frame (rip/rflags/user rsp,
     * plus the argument registers) is already saved in f by
     * entry.asm.
     */
    uint64_t saved_rbx, saved_rbp, saved_r12, saved_r13, saved_r14, saved_r15;
    __asm__ volatile (
        "mov %%rbx, %0\n\t"
        "mov %%rbp, %1\n\t"
        "mov %%r12, %2\n\t"
        "mov %%r13, %3\n\t"
        "mov %%r14, %4\n\t"
        "mov %%r15, %5"
        : "=r"(saved_rbx), "=r"(saved_rbp), "=r"(saved_r12),
          "=r"(saved_r13), "=r"(saved_r14), "=r"(saved_r15)
    );

    if (!g_current_proc) return -1;

    process_t *parent = g_current_proc;

    process_t *child = proc_create(parent->intent, parent->pid);
    if (!child) {
        serial_print("[FORK] proc_create failed\n");
        return -1;
    }

    /*
     * Build the register state the child resumes userspace with:
     * identical to the parent's at the moment of this syscall, except
     * rax = 0 (fork()'s return value in the child). rcx is left as a
     * plain general-purpose slot here, not the return address -- the
     * real SYSCALL instruction already destroys the pre-syscall rcx
     * on the parent's side too, so no fork() implementation can
     * preserve it; this matches that same, expected limitation.
     * child->context.rip points at fork_child_entry (above), which
     * restores this the same way sigreturn() resumes an interrupted
     * signal handler.
     */
    child->fork_frame.r15    = saved_r15;
    child->fork_frame.r14    = saved_r14;
    child->fork_frame.r13    = saved_r13;
    child->fork_frame.r12    = saved_r12;
    child->fork_frame.r11    = f->r11;
    child->fork_frame.r10    = f->r10;
    child->fork_frame.r9     = f->r9;
    child->fork_frame.r8     = f->r8;
    child->fork_frame.rbp    = saved_rbp;
    child->fork_frame.rdi    = f->rdi;
    child->fork_frame.rsi    = f->rsi;
    child->fork_frame.rdx    = f->rdx;
    child->fork_frame.rcx    = f->rcx;
    child->fork_frame.rbx    = saved_rbx;
    child->fork_frame.rax    = 0; /* fork() returns 0 in the child */
    child->fork_frame.rip    = f->rcx;    /* same instruction as parent */
    child->fork_frame.cs     = 0x23;      /* user code, see jump_userspace.asm */
    child->fork_frame.rflags = f->r11;
    child->fork_frame.rsp    = f->user_rsp;
    child->fork_frame.ss     = 0x1B;      /* user data, see jump_userspace.asm */

    child->context.rip = (uint64_t)(uintptr_t)fork_child_entry;

    uint64_t parent_pml4_phys;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(parent_pml4_phys));
    parent_pml4_phys &= VMM_ADDR_MASK;

    /*
     * BUG FIX: everything below walks page tables and copies pages by
     * dereferencing raw physical addresses (pmm_alloc() results, the
     * parent's own table entries) as plain kernel pointers. That only
     * works while the kernel's own raw PML4 is active, which has a
     * full 0-4GB identity map. The parent process actually runs on
     * its own isolated PML4 (see make_isolated_pml4() in elf.c),
     * which only keeps the low 32MB of physical memory identity
     * mapped, plus whatever specific pages the process has mapped for
     * its own segments/stack -- everything else pmm_alloc() can hand
     * out (which is most of RAM, since low memory fills up fast) is
     * simply absent from the parent's own page tables and faults the
     * instant it gets dereferenced here.
     *
     * Switch to the kernel's own PML4 for the whole physical-memory-
     * heavy part of the clone, and switch back to the parent's PML4
     * before returning, on every exit path (success and OOM alike) --
     * the parent must keep running on its own page tables once this
     * syscall returns.
     */
    uint64_t kpml4_phys = vmm_get_kernel_pml4();
    __asm__ volatile ("mov %0, %%cr3" :: "r"(kpml4_phys) : "memory");

    uint64_t child_pml4_phys = pmm_alloc(ZONE_GREEN);
    if (!child_pml4_phys) {
        __asm__ volatile ("mov %0, %%cr3" :: "r"(parent_pml4_phys) : "memory");
        proc_exit(child->pid, -1);
        return -1;
    }

    uint64_t *parent_pml4 = (uint64_t *)(uintptr_t)parent_pml4_phys;
    uint64_t *child_pml4  = (uint64_t *)(uintptr_t)child_pml4_phys;
    uint64_t *kpml4       = (uint64_t *)(uintptr_t)kpml4_phys;

    /*
     * FIX: Start by copying ALL 512 entries from the kernel PML4.
     * This ensures the kernel's low-half mappings (entry 0, where the
     * kernel code lives at ~0x100000) are present in the child's page
     * table alongside the upper-half kernel entries (256-511).
     * Without this, any exception in the child kills the kernel too.
     */
    for (int i = 0; i < 512; i++)
        child_pml4[i] = kpml4[i];

    /*
     * Deep-copy only pages that belong to the parent's user address
     * space — i.e. low-half entries that differ from the kernel PML4.
     */
    for (int p4 = 0; p4 < 256; p4++) {
        if (!(parent_pml4[p4] & 1)) continue;
        /* Skip entries inherited from the kernel PML4 */
        if (parent_pml4[p4] == kpml4[p4]) continue;

        uint64_t child_pdpt_phys = pmm_alloc(ZONE_GREEN);
        if (!child_pdpt_phys) goto oom;
        memset((void *)(uintptr_t)child_pdpt_phys, 0, PAGE_SIZE);
        child_pml4[p4] = child_pdpt_phys | (parent_pml4[p4] & 0xFFF);

        uint64_t *parent_pdpt = (uint64_t *)(uintptr_t)(parent_pml4[p4] & VMM_ADDR_MASK);
        uint64_t *child_pdpt  = (uint64_t *)(uintptr_t)child_pdpt_phys;

        for (int p3 = 0; p3 < 512; p3++) {
            if (!(parent_pdpt[p3] & 1)) continue;
            if (parent_pdpt[p3] & VMM_HUGE) {
                child_pdpt[p3] = parent_pdpt[p3];
                continue;
            }

            uint64_t child_pd_phys = pmm_alloc(ZONE_GREEN);
            if (!child_pd_phys) goto oom;
            memset((void *)(uintptr_t)child_pd_phys, 0, PAGE_SIZE);
            child_pdpt[p3] = child_pd_phys | (parent_pdpt[p3] & 0xFFF);

            uint64_t *parent_pd = (uint64_t *)(uintptr_t)(parent_pdpt[p3] & VMM_ADDR_MASK);
            uint64_t *child_pd  = (uint64_t *)(uintptr_t)child_pd_phys;

            for (int p2 = 0; p2 < 512; p2++) {
                if (!(parent_pd[p2] & 1)) continue;
                if (parent_pd[p2] & VMM_HUGE) {
                    child_pd[p2] = parent_pd[p2];
                    continue;
                }

                uint64_t child_pt_phys = pmm_alloc(ZONE_GREEN);
                if (!child_pt_phys) goto oom;
                memset((void *)(uintptr_t)child_pt_phys, 0, PAGE_SIZE);
                child_pd[p2] = child_pt_phys | (parent_pd[p2] & 0xFFF);

                uint64_t *parent_pt = (uint64_t *)(uintptr_t)(parent_pd[p2] & VMM_ADDR_MASK);
                uint64_t *child_pt  = (uint64_t *)(uintptr_t)child_pt_phys;

                for (int p1 = 0; p1 < 512; p1++) {
                    if (!(parent_pt[p1] & 1)) continue;

                    uint64_t src_phys = parent_pt[p1] & VMM_ADDR_MASK;
                    uint64_t dst_phys = pmm_alloc(ZONE_RED);
                    if (!dst_phys) goto oom;

                    memcpy((void *)(uintptr_t)dst_phys,
                           (void *)(uintptr_t)src_phys, PAGE_SIZE);

                    child_pt[p1] = dst_phys | (parent_pt[p1] & 0xFFF);
                }
            }
        }
    }

    child->cr3 = child_pml4_phys;

    /* Done touching physical memory directly: back to the parent's
     * own page tables before anything below (or the caller, once this
     * syscall returns) runs. */
    __asm__ volatile ("mov %0, %%cr3" :: "r"(parent_pml4_phys) : "memory");

    memcpy(child->caps, parent->caps,
           parent->cap_count * sizeof(cap_entry_t));
    child->cap_count = parent->cap_count;

    /*
     * context.rip is already fork_child_entry, set above -- only rsp
     * needs fixing up here, to the child's own fresh kernel stack
     * (context.rsp is a kernel-stack pointer used by context_switch
     * to resume this process's C call chain, unrelated to
     * fork_frame.rsp above, which is the *user*-mode stack pointer
     * fork_child_entry hands off to iretq).
     */
    child->context.rsp = child->kernel_stack_top;

    child->state = PROC_READY;
    sched_enqueue(child);

    audit_record(AUDIT_PROC_FORK, child->pid, parent->pid, 0);

    serial_print("[FORK] Child PID=");
    serial_printhex((uint64_t)child->pid);
    serial_print(" created from PID=");
    serial_printhex((uint64_t)parent->pid);
    serial_print("\n");

    return (int64_t)child->pid;

oom:
    serial_print("[FORK] OOM during address space clone\n");
    free_user_address_space(child_pml4_phys);
    child->cr3 = 0;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(parent_pml4_phys) : "memory");
    proc_exit(child->pid, -1);
    return -1;
}


/*  exec()    */


int64_t sys_exec_impl(const uint8_t *elf_data, uint64_t elf_size)
{
    if (!g_current_proc) return -1;

    uint64_t new_pml4 = 0;
    uint64_t entry    = 0;
    uint64_t stk_top  = 0;

    if (!elf_load(elf_data, elf_size, &new_pml4, &entry, &stk_top, (const char**)0, (const char**)0)) {
        serial_print("[EXEC] ELF load failed\n");
        return -1;
    }

    g_current_proc->cr3 = new_pml4;

    audit_record(AUDIT_PROC_EXEC, g_current_proc->pid, entry, 0);

    serial_print("[EXEC] Jumping to user entry=");
    serial_printhex(entry);
    serial_print("\n");

    /*
     * FIX: Do NOT switch CR3 here. jump_to_userspace() does the CR3
     * switch internally, right before iretq, while still executing
     * on the kernel stack. Switching CR3 here would immediately
     * unmap the kernel code (including jump_to_userspace itself),
     * causing an instant instruction-fetch page fault.
     */
    __asm__ volatile("cli\njmp jump_to_userspace\n" : : "D"(entry),"S"(stk_top),"d"(new_pml4));

    /* unreachable */
    return 0;
}


/*  waitpid()   */


int64_t sys_waitpid_impl(uint32_t pid)
{
    process_t *child = proc_get(pid);
    if (!child) return -1;
    if (child->state != PROC_ZOMBIE && g_current_proc) {
        g_current_proc->blocked_on_pid = pid;
        g_current_proc->state          = PROC_BLOCKED;
        sched_yield();
    }
    child = proc_get(pid);
    if (!child) return -1;
    int code = child->exit_code;
    child->state = PROC_UNUSED;
    child->pid   = 0;
    return (int64_t)code;
}