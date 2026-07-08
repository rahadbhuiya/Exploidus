#pragma once
#include <stdint.h>

/*
 * kernel/arch/x86_64/fpu.h — enables hardware FPU/SSE and provides
 * per-process FXSAVE/FXRSTOR state buffers.
 *
 * Why this didn't exist before: the kernel build uses -mno-sse
 * -mno-mmx -mno-sse2 everywhere, which is *why* nothing (including a
 * math.h) could return a `double` — the x86-64 System V ABI returns
 * floating-point values in XMM0, so without SSE enabled the compiler
 * has no legal way to return one at all. That flag was there because
 * context_switch.asm didn't save/restore FPU/XMM register state, so
 * enabling SSE without this would have let one process's floating
 * point silently corrupt another's on every context switch.
 *
 * fpu_init() enables SSE at the CPU level (needed process-wide, since
 * the fault-on-use behavior is a CPU/CR0/CR4 setting, not per-process)
 * and captures a valid "reset" FXSAVE image once at boot. Every new
 * process gets its own 512-byte, 16-byte-aligned state buffer
 * (allocated via fpu_alloc_state()), initialized from that template.
 * The scheduler FXSAVEs/FXRSTORs per-process around every context
 * switch (see scheduler.c) — done there in C, not in
 * context_switch.asm, so the pointer is resolved by the compiler
 * instead of a hand-computed asm offset.
 */

#define FPU_STATE_SIZE 512  /* FXSAVE/FXRSTOR area size, fixed by the ISA */

void fpu_init(void);

/* Allocates a new 16-byte-aligned FXSAVE area for a process,
 * initialized from the boot-time reset template. Returns NULL on
 * allocation failure. */
uint8_t *fpu_alloc_state(void);

/* Frees a state buffer returned by fpu_alloc_state(). */
void fpu_free_state(uint8_t *state);