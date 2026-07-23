#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../cap/capability.h"
#include "../ipc/ipc.h"     /* ipc_state_t */
#include "../arch/x86_64/idt.h" /* interrupt_frame_t, for sig_saved_frame */

#define MAX_PROCESSES     256
#define MAX_CAPS_PER_PROC 64
#define KERNEL_STACK_SIZE (16 * 1024)  /* 16 KiB per process kernel stack */

typedef enum {
    PROC_UNUSED    = 0,
    PROC_RUNNING   = 1,
    PROC_READY     = 2,
    PROC_BLOCKED   = 3,
    PROC_ZOMBIE    = 4,
} proc_state_t;

typedef enum {
    INTENT_COMPUTE     = 0,
    INTENT_IO          = 1,
    INTENT_NETWORK     = 2,
    INTENT_INTERACTIVE = 3,
    INTENT_AUDIT       = 4,
    INTENT_COUNT       = 5
} proc_intent_t;

/*
 * cpu_context_t — callee-saved registers + RIP.
 * Layout must exactly match context_switch.asm.
 *
 * Byte offsets (do NOT reorder — entry.asm and context_switch.asm depend on these):
 *   r15=0  r14=8  r13=16  r12=24  rbx=32  rbp=40  rsp=48  rip=56
 */
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rsp;
    uint64_t rip;
} cpu_context_t;  /* 64 bytes total */

/*
 * process_t byte layout (entry.asm hardcodes offset 96 for kernel_stack_top):
 *   pid(4) + parent_pid(4)              =  8  -> total  8
 *   state(4) + intent(4)                =  8  -> total 16
 *   cpu_context_t                       = 64  -> total 80
 *   cr3(8)                              =  8  -> total 88
 *   *kernel_stack(8)                    =  8  -> total 96
 *   kernel_stack_top(8)                 =  8  -> total 104  <-- offset 96
 */
typedef struct process {
    uint32_t       pid;            /* offset   0 */
    uint32_t       parent_pid;     /* offset   4 */
    proc_state_t   state;          /* offset   8 */
    proc_intent_t  intent;         /* offset  12 */

    cpu_context_t  context;        /* offset  16 (64 bytes) */

    uint64_t       cr3;            /* offset  80 */
    uint8_t       *kernel_stack;   /* offset  88 */
    uint64_t       kernel_stack_top; /* offset 96 */

    cap_entry_t    caps[MAX_CAPS_PER_PROC];
    uint32_t       cap_count;

    uint64_t       ticks_used;
    int            exit_code;

    /* mmap region grows down from this address */
    uint64_t       mmap_top;

    /* non-zero when blocked in waitpid() */
    uint32_t       blocked_on_pid;
    uint64_t       wake_tick;      /* for sleep — unblock when uptime >= wake_tick */

    uint64_t       user_entry;
    uint64_t       user_stack_top;
    struct process *sched_next;

    /* GUI / IPC — added after sched_next so all prior offsets are unchanged */
    ipc_state_t   *ipc;            /* per-process IPC inbox, alloc'd at create */

    /* Thread-local storage base (x86-64 FS segment base). Restored by
     * the scheduler on every context switch via wrmsr so ported
     * language runtimes (which almost universally expect TLS to work,
     * even single-threaded) have somewhere to point %fs. Appended
     * last for the same reason as ipc above — safe, no offset reuse. */
    uint64_t       fs_base;

    /* FXSAVE/FXRSTOR area (512 bytes, 16-byte aligned) — see
     * kernel/arch/x86_64/fpu.h/.c. Appended last, same reasoning as
     * ipc/fs_base above. */
    uint8_t       *fpu_state;

    /* Registered signal handlers (userspace addresses), indexed by
     * signal number — see kernel/arch/x86_64/idt.c for delivery.
     * 0 = no handler registered (default action: kill the process). */
    uint64_t       sig_handlers[16];

    /* Signal-resume state (see kernel/arch/x86_64/idt.c delivery and
     * sys_sigreturn() in kernel/syscall/table.c). Only one saved
     * frame slot exists, so sig_in_progress is the reentrancy guard:
     * a fault that happens while a handler is already running always
     * falls through to the kill path instead of nesting and
     * clobbering the slot. Appended last, same reasoning as
     * ipc/fs_base/fpu_state above: safe, no offset reuse. */
    bool               sig_in_progress;
    interrupt_frame_t  sig_saved_frame;

    /* Signals set by kill() targeting this process (bit i = signal i
     * pending), delivered the next time this process is about to
     * resume in userspace -- see deliver_pending_signal() in
     * kernel/arch/x86_64/idt.c and sys_kill() in
     * kernel/syscall/table.c. */
    uint32_t           pending_signals;

    /* Register state a freshly-forked child should resume userspace
     * with (identical to the parent's at the moment of fork(), except
     * rax = 0). Set by sys_fork_impl() in kernel/proc/fork_exec.c,
     * consumed once by fork_child_entry() the first time the
     * scheduler runs this process. */
    interrupt_frame_t  fork_frame;
} process_t;

void       proc_init(void);
process_t *proc_create(proc_intent_t intent, uint32_t parent_pid);
process_t *proc_get(uint32_t pid);
void       proc_exit(uint32_t pid, int code);
bool       proc_add_cap(uint32_t pid, cap_token_t token,
                        uint32_t owner_pid, bool delegatable);

const process_t *proc_get_table(uint32_t *count_out);

/* Currently running process */
extern process_t *g_current_proc;

/*
 * g_kernel_stack_top — kernel_stack_top of the currently running process.
 * Updated by the scheduler via make_current() whenever g_current_proc changes.
 * Used by syscall entry.asm to switch stacks without a struct offset calculation.
 */
extern uint64_t g_kernel_stack_top;