#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../cap/capability.h"

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