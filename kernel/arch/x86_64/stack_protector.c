#include <stdint.h>
#include "../../drivers/serial.h"

/*
 * -fstack-protector-strong (enabled in the Makefile) makes GCC emit
 * a reference to __stack_chk_guard at the top of every instrumented
 * function (saved onto the stack, above any local buffers) and a
 * call to __stack_chk_fail() right before return if the saved copy
 * no longer matches -- i.e. something on the stack overran into it.
 * Freestanding/no-libc means nothing provides these two symbols
 * automatically; this file does.
 *
 * __stack_chk_guard starts as a fixed, non-zero placeholder (better
 * than 0, which would make a NUL-terminated-string buffer overflow
 * that happens to stop exactly at the canary go undetected) and gets
 * replaced with a real RDRAND-sourced value by stack_protector_init(),
 * called from kernel/main.c as close to the very first line as
 * possible. Anything that runs before that call is still checked,
 * just against the weaker placeholder rather than a random value.
 */
uintptr_t __stack_chk_guard = 0xBADC0FFEE0DDF00DULL;

static inline int has_rdrand(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (ecx & (1 << 30)) != 0;
}

static uint64_t sp_rdrand64(void)
{
    if (!has_rdrand()) {
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32 | lo) ^ 0x5A5A5A5A5A5A5A5AULL;
    }
    uint64_t val = 0;
    uint8_t ok = 0;
    for (int i = 0; i < 10; i++) {
        __asm__ volatile ("rdrand %0\n\t setc %1\n\t" : "=r"(val), "=qm"(ok));
        if (ok) break;
    }
    return val;
}

void stack_protector_init(void)
{
    uint64_t g = sp_rdrand64();
    /* Never let the guard be all-zero, and always force its low byte
     * to non-zero -- matching glibc's own stack-canary convention:
     * a 0x00 low byte means a NUL-terminated string overflow that
     * stops exactly at the canary's first byte would print/compare
     * as if the canary were untouched, silently defeating the check
     * for that specific (common) overflow pattern. */
    if (g == 0) g = 0x1234567890ABCDEFULL;
    g |= 0x01ULL;
    __stack_chk_guard = (uintptr_t)g;
}

__attribute__((noreturn))
void __stack_chk_fail(void)
{
    serial_print("\n[PANIC] Stack smashing detected -- halting.\n");
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}