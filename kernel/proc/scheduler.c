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

void sched_tick(void)
{
    g_uptime_ticks++;
    unblock_waiters();
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
        /* Shutdown QEMU via ACPI */
        __asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }

    sched_dequeue(next);
    make_current(next);
    g_ticks_remaining = SLICES[next->intent];
    next->state = PROC_RUNNING;

    if (next == prev)
        return;

    serial_printhex((uint64_t)next->pid);
    serial_print("\n");
    context_switch(prev, next);
}
