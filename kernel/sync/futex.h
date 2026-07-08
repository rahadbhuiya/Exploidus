#pragma once
#include <stdint.h>

/*
 * kernel/sync/futex.h — address-based wait/wake primitive, in the
 * spirit of Linux's futex(2) but deliberately scoped down:
 *
 *   - Only synchronizes within the SAME process (matched by pid).
 *     Exploidus doesn't have shared-address-space threads yet (every
 *     process still has its own address space), so a truly general
 *     cross-address-space futex would need physical-address
 *     resolution via a page-table walker that doesn't exist yet.
 *     Scoping to same-pid covers the realistic near-term use case: a
 *     ported language runtime building its own mutex/condvar on top
 *     of this within one process.
 *   - No FUTEX_PRIVATE/SHARED flag, no timeout — callers needing a
 *     timeout should re-check with their own wake_tick-based sleep.
 *
 * futex_wait(addr, expected): if the current value at *addr no longer
 * equals `expected`, returns immediately (someone already changed it
 * — standard futex semantics avoid a lost-wakeup race). Otherwise
 * blocks the calling process until a matching futex_wake().
 *
 * futex_wake(addr, count): wakes up to `count` processes (0 = none,
 * a very large count = "wake all") currently blocked via
 * futex_wait() on that same address, in the calling process.
 */

void futex_wait(uint64_t uaddr, uint32_t expected);
void futex_wake(uint64_t uaddr, uint32_t count);