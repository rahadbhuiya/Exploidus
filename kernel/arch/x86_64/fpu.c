#include "fpu.h"
#include "../../mm/kmalloc.h"
#include <string.h>

static uint8_t g_fpu_template[FPU_STATE_SIZE] __attribute__((aligned(16)));

void fpu_init(void)
{
    uint64_t cr0, cr4;

    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2); /* EM = 0: don't trap on x87/SSE instructions */
    cr0 |=  (1ULL << 1); /* MP = 1: required alongside EM=0 for FPU    */
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));

    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);  /* OSFXSR: enables FXSAVE/FXRSTOR + SSE       */
    cr4 |= (1ULL << 10); /* OSXMMEXCPT: unmasked SIMD FP exceptions    */
    __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4));

    /* Capture a valid reset state now that SSE is live — every new
     * process's FPU state starts as a copy of this. */
    __asm__ volatile ("fxsave (%0)" :: "r"(g_fpu_template) : "memory");
}

uint8_t *fpu_alloc_state(void)
{
    uint8_t *raw = (uint8_t *)kmalloc(FPU_STATE_SIZE + 32);
    if (!raw) return NULL;

    uintptr_t base    = (uintptr_t)raw + 16;
    uintptr_t aligned = (base + 15) & ~(uintptr_t)15;

    /* Stash the original allocation pointer just below the aligned
     * area (guaranteed >= 8 bytes of headroom by construction above)
     * so fpu_free_state() can recover it for kfree(). */
    *(uint8_t **)(aligned - sizeof(uint8_t *)) = raw;

    memcpy((void *)aligned, g_fpu_template, FPU_STATE_SIZE);
    return (uint8_t *)aligned;
}

void fpu_free_state(uint8_t *state)
{
    if (!state) return;
    uint8_t *raw = *(uint8_t **)((uintptr_t)state - sizeof(uint8_t *));
    kfree(raw);
}