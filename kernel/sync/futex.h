#pragma once
#include <stdint.h>

/*
 * kernel/sync/futex.h — address-based wait/wake primitive, in the
 * spirit of Linux's futex(2) but deliberately scoped down:
 * - Only synchronizes within the SAME process (matched by pid).
 * Exploidus doesn't have shared-address-space threads yet (every
 */

void futex_wait(uint64_t uaddr, uint32_t expected);
void futex_wake(uint64_t uaddr, uint32_t count);