#pragma once
#include <stdint.h>

/*
 * kernel/sync — synchronization primitives.
 *
 * Exploidus is currently single-core, so there is no true parallel
 * execution — but there IS concurrency: an IRQ (timer, keyboard,
 * mouse, network card, ...) can interrupt kernel code at almost any
 * point. If that kernel code was in the middle of mutating a shared
 * data structure (like the kmalloc free-list), and the IRQ handler
 * touches the same structure, it gets corrupted. That's a real bug
 * class here, not a theoretical one — kmalloc()/kfree() had exactly
 * zero protection against it before this file existed.
 *
 * Two different tools for two different problems:
 *
 *   spinlock_t   — for data that can be touched from IRQ context
 *                  (allocators, driver rings, shared kernel tables).
 *                  Implemented as interrupt-disable, since with one
 *                  core that's sufficient and it's what makes it safe
 *                  to take from an IRQ handler itself. NEVER blocks.
 *
 *   mutex_t /
 *   semaphore_t  — for process-vs-process contention over a resource
 *                  where waiting is fine (e.g. two processes sharing
 *                  an IPC channel or a file). These actually block the
 *                  waiting process (PROC_BLOCKED + sched_yield()) so
 *                  it burns no CPU while waiting, matching the
 *                  existing blocking-waitpid design elsewhere in the
 *                  kernel. MUST NOT be called from IRQ context.
 */


/* Spinlock (IRQ-disable based — single-core safe, IRQ-context safe) */


typedef struct {
    volatile int locked;
} spinlock_t;

void     spin_init(spinlock_t *l);

/* Disables interrupts and acquires the lock; returns the previous
 * RFLAGS value so it can be restored exactly (nested-safe) by
 * spin_unlock_irqrestore(). */
uint64_t spin_lock_irqsave(spinlock_t *l);
void     spin_unlock_irqrestore(spinlock_t *l, uint64_t saved_flags);


/* Mutex (blocking — process context only) */


#define MUTEX_MAX_WAITERS 32

typedef struct {
    volatile int locked;
    uint32_t     owner_pid;
    uint32_t     waiters[MUTEX_MAX_WAITERS];
    int          waiter_count;
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);      /* blocks the calling process until acquired */
int  mutex_trylock(mutex_t *m);   /* 1 = acquired, 0 = already held */
void mutex_unlock(mutex_t *m);


/* Counting semaphore (blocking — process context only) */


#define SEM_MAX_WAITERS 32

typedef struct {
    volatile int count;
    uint32_t     waiters[SEM_MAX_WAITERS];
    int          waiter_count;
} semaphore_t;

void sem_init(semaphore_t *s, int initial_count);
void sem_wait(semaphore_t *s);    /* blocks the calling process until count > 0 */
void sem_post(semaphore_t *s);