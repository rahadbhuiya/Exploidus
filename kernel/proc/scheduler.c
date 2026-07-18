#include "scheduler.h"
#include "process.h"
#include "../arch/x86_64/gdt.h"
#include "../drivers/serial.h"
#include <string.h>

extern void context_switch(process_t *prev, process_t *next);

static const int QUEUE_ORDER[INTENT_COUNT] = {
    INTENT_AUDIT,
    INTENT_INTERACTIVE,
    INTENT_IO,
    INTENT_NETWORK,
    INTENT_COMPUTE,
};

static const uint32_t SLICES[INTENT_COUNT] = {
    [INTENT_COMPUTE]     = SLICE_COMPUTE,
    [INTENT_IO]          = SLICE_IO,
    [INTENT_NETWORK]     = SLICE_NETWORK,
    [INTENT_INTERACTIVE] = SLICE_INTERACTIVE,
    [INTENT_AUDIT]       = SLICE_AUDIT,
};

static process_t *g_queues[INTENT_COUNT];
static uint32_t   g_ticks_remaining = 0;
uint64_t g_uptime_ticks = 0;

void sched_init(void)
{
    memset(g_queues, 0, sizeof(g_queues));
    g_ticks_remaining = 0;
    serial_print("[SCHED] initialized\n");
}

void sched_enqueue(process_t *p)
{
    if (!p || p->intent >= INTENT_COUNT) return;

    /* Check if already in queue */
    { process_t *chk = g_queues[p->intent];
      while (chk) {
          if (chk == p) {
              return;
          }
          chk = chk->sched_next;
      }
    }

    p->sched_next = NULL;

    if (!g_queues[p->intent]) {
        g_queues[p->intent] = p;
        return;
    }

    process_t *cur = g_queues[p->intent];
    while (cur->sched_next)
        cur = cur->sched_next;

    cur->sched_next = p;
}

void sched_dequeue(process_t *p)
{
    if (!p || p->intent >= INTENT_COUNT) return;

    process_t *cur  = g_queues[p->intent];
    process_t *prev = NULL;

    while (cur) {
        if (cur == p) {
            if (prev) prev->sched_next = cur->sched_next;
            else      g_queues[p->intent] = cur->sched_next;
            cur->sched_next = NULL;
            return;
        }
        prev = cur;
        cur  = cur->sched_next;
    }
}

process_t *sched_next(void)
{
    for (int i = 0; i < INTENT_COUNT; i++) {
        int q = QUEUE_ORDER[i];
        if (g_queues[q])
            return g_queues[q];
    }
    return NULL;
}

/*
 * make_current — set p as the running process and update all
 * CPU-facing state that must track the current process:
 *   1. g_current_proc          (used by syscall_dispatch and C code)
 *   2. g_kernel_stack_top      (used by entry.asm for stack switch)
 *   3. TSS RSP0                (used by CPU on any ring-3 interrupt)
 */
static void make_current(process_t *p)
{
    g_current_proc       = p;
    g_kernel_stack_top   = p->kernel_stack_top;
    g_syscall_kernel_rsp = p->kernel_stack_top;
    tss_set_rsp0(p->kernel_stack_top);
}

/*
 * unblock_waiters — scan for processes blocked in waitpid() whose
 * child has become a zombie, and move them back to READY.
 */
static void unblock_waiters(void)
{
    uint32_t count;
    const process_t *table = proc_get_table(&count);

    for (uint32_t i = 0; i < count; i++) {
        process_t *p = (process_t *)&table[i];  /* cast away read-only */
        if (p->state != PROC_BLOCKED || !p->blocked_on_pid)
            continue;

        process_t *child = proc_get(p->blocked_on_pid);
        if (!child || child->state == PROC_ZOMBIE) {
            p->blocked_on_pid = 0;
            p->state          = PROC_READY;
            sched_enqueue(p);
        }
    }
}

static void unblock_sleepers(void)
{
    uint32_t count;
    const process_t *table = proc_get_table(&count);

    for (uint32_t i = 0; i < count; i++) {
        process_t *p = (process_t *)&table[i];
        if (p->state != PROC_BLOCKED || p->wake_tick == 0)
            continue;
        if (g_uptime_ticks >= p->wake_tick) {
            p->wake_tick = 0;
            p->state     = PROC_READY;
            sched_enqueue(p);
            /* NOTE: do NOT serial_print here. This function runs from
             * the timer IRQ handler on every tick (100Hz) for every
             * process that just woke up. serial_putc() busy-waits on
             * real UART transmit timing (~90us/byte) — with interrupts
             * effectively stalled for that whole time, every sleeping
             * process waking up was enough to noticeably delay
             * keyboard/mouse IRQs and scheduling system-wide. */
        }
    }
}

void sched_tick(void)
{
    g_uptime_ticks++;

    /*
     * Per-process CPU time accounting — this field (ticks_used) was
     * already declared on process_t and exposed to userspace via
     * getprocs()/the 'ps' shell command's TICKS column, but nothing
     * anywhere in the kernel ever incremented it, so 'ps' always
     * showed 0 for every process no matter how much CPU it had
     * actually used. Simple sampling: whichever process is current
     * when each 100Hz timer tick fires gets credited one tick — the
     * standard, lightweight way OS-level CPU-time accounting works.
     */
    if (g_current_proc) g_current_proc->ticks_used++;

    unblock_sleepers();
    unblock_waiters();
    /* Poll network on every tick so TCP/IP works while processes run */
    extern void net_poll(void);
    net_poll();
}

void sched_yield(void)
{
    if (!g_current_proc) return;

    __asm__ volatile ("cli" ::: "memory");

    process_t *prev = g_current_proc;
    bool requeue_prev = (prev->state == PROC_RUNNING);

    if (requeue_prev) {
        prev->state = PROC_READY;
        sched_enqueue(prev);
    }

    g_current_proc = NULL;
    unblock_waiters();

    process_t *next = sched_next();
    if (!next) {
        if (requeue_prev) {
            make_current(prev);
            prev->state = PROC_RUNNING;
            __asm__ volatile ("sti" ::: "memory");
            return;
        }
        /* No runnable process — idle until timer wakes something */
        while (1) {
            __asm__ volatile ("sti; hlt; cli" ::: "memory");
            unblock_sleepers();
            unblock_waiters();
            next = sched_next();
            if (next) break;
        }
        serial_print("[SCHED] woke up, running PID=");
        serial_printhex((uint64_t)next->pid);
        serial_print("\n");
    }

    sched_dequeue(next);
    make_current(next);
    g_ticks_remaining = SLICES[next->intent];
    next->state = PROC_RUNNING;

    if (next == prev)
        return;

    /*
     * Save/restore FPU-SSE state (FXSAVE/FXRSTOR) around the switch —
     * same reasoning as the FS_BASE restore just above: done here in
     * C, not in context_switch.asm, so the compiler resolves the
     * fpu_state pointer's offset instead of it being hand-computed.
     * Must happen before the FS_BASE/TLS restore is irrelevant here,
     * but does need prev's save to happen while prev's FPU registers
     * are still live (i.e. now, before anything below touches SSE —
     * nothing here does).
     */
    if (prev && prev->fpu_state) {
        __asm__ volatile ("fxsave (%0)" :: "r"(prev->fpu_state) : "memory");
    }
    if (next->fpu_state) {
        __asm__ volatile ("fxrstor (%0)" :: "r"(next->fpu_state) : "memory");
    }

    /*
     * Restore TLS base (FS_BASE MSR) for `next` before switching to it.
     * Done here in C (compiler resolves the real struct offset) rather
     * than in context_switch.asm, which hardcodes byte offsets for the
     * fixed-layout prefix of process_t — fs_base was appended after
     * that prefix specifically so nothing there needs to change.
     * Safe to set before the actual switch: kernel code here doesn't
     * use %fs itself, so changing it early doesn't affect `prev`.
     */
    {
        uint32_t lo = (uint32_t)next->fs_base;
        uint32_t hi = (uint32_t)(next->fs_base >> 32);
        __asm__ volatile ("wrmsr" :: "c"(0xC0000100u), "a"(lo), "d"(hi));
    }

    context_switch(prev, next);
}