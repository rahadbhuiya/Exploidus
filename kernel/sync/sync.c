#include "sync.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"


/* Spinlock */


void spin_init(spinlock_t *l)
{
    l->locked = 0;
}

uint64_t spin_lock_irqsave(spinlock_t *l)
{
    uint64_t flags;
    __asm__ volatile (
        "pushfq\n\t"
        "pop %0\n\t"
        "cli"
        : "=r"(flags) :: "memory"
    );

    /* Single core: once interrupts are off, nothing else can run, so
     * the lock can never actually be contended here. The 'locked'
     * flag still exists to make double-acquire / use-after-init bugs
     * detectable if this ever grows SMP support. */
    l->locked = 1;
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *l, uint64_t saved_flags)
{
    l->locked = 0;
    if (saved_flags & (1ULL << 9)) { /* IF bit */
        __asm__ volatile ("sti" ::: "memory");
    }
}


/* Mutex — blocking, process context only */


void mutex_init(mutex_t *m)
{
    m->locked       = 0;
    m->owner_pid    = 0;
    m->waiter_count = 0;
}

int mutex_trylock(mutex_t *m)
{
    __asm__ volatile ("cli" ::: "memory");
    int got = 0;
    if (!m->locked) {
        m->locked    = 1;
        m->owner_pid = g_current_proc ? g_current_proc->pid : 0;
        got = 1;
    }
    __asm__ volatile ("sti" ::: "memory");
    return got;
}

void mutex_lock(mutex_t *m)
{
    for (;;) {
        __asm__ volatile ("cli" ::: "memory");

        if (!m->locked) {
            m->locked    = 1;
            m->owner_pid = g_current_proc ? g_current_proc->pid : 0;
            __asm__ volatile ("sti" ::: "memory");
            return;
        }

        /* Already held by someone else: register as a waiter and go
         * to sleep. sched_yield() only re-enqueues the calling process
         * if its state is still PROC_RUNNING when called — marking it
         * BLOCKED first means it will NOT be scheduled again until
         * mutex_unlock() explicitly wakes it. */
        process_t *me = g_current_proc;
        if (me && m->waiter_count < MUTEX_MAX_WAITERS) {
            m->waiters[m->waiter_count++] = me->pid;
            me->state = PROC_BLOCKED;
        }
        __asm__ volatile ("sti" ::: "memory");

        sched_yield();
        /* Woken up (or nothing to do but retry) — loop and re-check. */
    }
}

void mutex_unlock(mutex_t *m)
{
    __asm__ volatile ("cli" ::: "memory");

    m->locked    = 0;
    m->owner_pid = 0;

    /* Wake every current waiter; they'll re-race for the lock via the
     * loop in mutex_lock(). Simple "wake all" instead of hand-off to
     * one specific waiter — costs a little extra scheduling churn
     * under heavy contention, but is much harder to get wrong. */
    for (int i = 0; i < m->waiter_count; i++) {
        process_t *p = proc_get(m->waiters[i]);
        if (p && p->state == PROC_BLOCKED) {
            p->state = PROC_READY;
            sched_enqueue(p);
        }
    }
    m->waiter_count = 0;

    __asm__ volatile ("sti" ::: "memory");
}


/* Counting semaphore — blocking, process context only */


void sem_init(semaphore_t *s, int initial_count)
{
    s->count        = initial_count;
    s->waiter_count = 0;
}

void sem_wait(semaphore_t *s)
{
    for (;;) {
        __asm__ volatile ("cli" ::: "memory");

        if (s->count > 0) {
            s->count--;
            __asm__ volatile ("sti" ::: "memory");
            return;
        }

        process_t *me = g_current_proc;
        if (me && s->waiter_count < SEM_MAX_WAITERS) {
            s->waiters[s->waiter_count++] = me->pid;
            me->state = PROC_BLOCKED;
        }
        __asm__ volatile ("sti" ::: "memory");

        sched_yield();
    }
}

void sem_post(semaphore_t *s)
{
    __asm__ volatile ("cli" ::: "memory");

    s->count++;

    for (int i = 0; i < s->waiter_count; i++) {
        process_t *p = proc_get(s->waiters[i]);
        if (p && p->state == PROC_BLOCKED) {
            p->state = PROC_READY;
            sched_enqueue(p);
        }
    }
    s->waiter_count = 0;

    __asm__ volatile ("sti" ::: "memory");
}