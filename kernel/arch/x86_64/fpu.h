#pragma once
#include <stdint.h>

/*
 * kernel/arch/x86_64/fpu.h — enables hardware FPU/SSE and provides
 * per-process FXSAVE/FXRSTOR state buffers.
 * Why this didn't exist before: the kernel build uses -mno-sse
 * -mno-mmx -mno-sse2 everywhere, which is *why* nothing (including a
 */

#define FPU_STATE_SIZE 512  /* FXSAVE/FXRSTOR area size, fixed by the ISA */

void fpu_init(void);

/* Allocates a new 16-byte-aligned FXSAVE area for a process,
 * initialized from the boot-time reset template. Returns NULL on
 * allocation failure. */
uint8_t *fpu_alloc_state(void);

/* Frees a state buffer returned by fpu_alloc_state(). */
void fpu_free_state(uint8_t *state);