#include "futex.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"

#define FUTEX_MAX_WAITERS 64

typedef struct {
    int      used;
    uint32_t owner_pid;   /* whose address space uaddr belongs to */
    uint64_t uaddr;
    uint32_t waiter_pid;
} futex_entry_t;

static futex_entry_t g_futex_table[FUTEX_MAX_WAITERS];

void futex_wait(uint64_t uaddr, uint32_t expected)
{
    if (!g_current_proc) return;

    __asm__ volatile ("cli" ::: "memory");

    uint32_t current_val = *(volatile uint32_t *)(uintptr_t)uaddr;
    if (current_val != expected) {
        /* Value already changed since the caller last checked it —
         * don't block, matches standard futex semantics and avoids a
         * lost-wakeup race between check and wait. */
        __asm__ volatile ("sti" ::: "memory");
        return;
    }

    int slot = -1;
    for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
        if (!g_futex_table[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        /* Waiter table full: fall back to a plain yield rather than
         * failing outright. Caller loops and re-checks anyway. */
        __asm__ volatile ("sti" ::: "memory");
        sched_yield();
        return;
    }

    g_futex_table[slot].used       = 1;
    g_futex_table[slot].owner_pid  = g_current_proc->pid;
    g_futex_table[slot].uaddr      = uaddr;
    g_futex_table[slot].waiter_pid = g_current_proc->pid;

    g_current_proc->state = PROC_BLOCKED;

    __asm__ volatile ("sti" ::: "memory");

    sched_yield();

    /* Back here after being woken (futex_wake already cleared our
     * slot) or after a spurious resume. Either way it's the caller's
     * job (building a mutex/condvar on top) to re-check its condition
     * and call futex_wait() again if it's still not satisfied. */
}

void futex_wake(uint64_t uaddr, uint32_t count)
{
    if (!g_current_proc) return;

    __asm__ volatile ("cli" ::: "memory");

    uint32_t owner = g_current_proc->pid;
    uint32_t woken = 0;

    for (int i = 0; i < FUTEX_MAX_WAITERS && woken < count; i++) {
        if (g_futex_table[i].used &&
            g_futex_table[i].owner_pid == owner &&
            g_futex_table[i].uaddr == uaddr) {

            process_t *p = proc_get(g_futex_table[i].waiter_pid);
            if (p && p->state == PROC_BLOCKED) {
                p->state = PROC_READY;
                sched_enqueue(p);
            }
            g_futex_table[i].used = 0;
            woken++;
        }
    }

    __asm__ volatile ("sti" ::: "memory");
}