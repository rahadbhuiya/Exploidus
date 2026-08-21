#include <stdint.h>
#include "syscall.h"

/*
 * Userspace counterpart to kernel/arch/x86_64/stack_protector.c --
 * see that file's comment for the full explanation of why these
 * symbols need to exist at all under -fstack-protector-strong in a
 * freestanding/no-libc environment.
 *
 * Every userspace binary needs its OWN copy of this (it's a global
 * per-process, not something the kernel can share out), which is why
 * this lives in userspace/libc rather than being kernel-provided --
 * including the minimal shell binary, which doesn't link the rest of
 * libc (see the Makefile: shell links crt0+setjmp+its own .c files
 * only, not $(LIBC_OBJS)) but still needs this object explicitly
 * added to its link line.
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

/*
 * Called from crt0.asm before main(), as close to process start as
 * possible -- same reasoning as the kernel side: anything that ran
 * before this (there isn't much, in practice just crt0 itself) is
 * still checked, just against the weaker startup placeholder.
 */
void __stack_chk_init(void)
{
    uint64_t g = sp_rdrand64();
    if (g == 0) g = 0x1234567890ABCDEFULL;
    g |= 0x01ULL; /* never let the low byte be 0x00 -- see kernel-side
                    * comment for why (NUL-terminated string overflow
                    * evasion) */
    __stack_chk_guard = (uintptr_t)g;
}

__attribute__((noreturn))
void __stack_chk_fail(void)
{
    /* Deliberately raw syscalls here, not puts()/exit() -- those are
     * #ifndef __EXPLOIDUS_LIBC__ guarded in syscall.h (this file
     * compiles WITH that flag set, as part of LIBC_C_SRCS, since
     * stdio.c provides its own real puts() and a name clash would
     * otherwise result), so they aren't visible from here. */
    static const char msg[] = "\n*** stack smashing detected ***\n";
    syscall3(SYS_WRITE, 2 /* stderr */, (uint64_t)(uintptr_t)msg,
             sizeof(msg) - 1);
    syscall1(SYS_EXIT, 134); /* 128 + SIGABRT(6), matching glibc's
                                * convention for how a stack-smashing
                                * abort's exit code looks to a
                                * parent/shell waiting on this
                                * process */
    for (;;) { } /* satisfy noreturn -- SYS_EXIT never actually returns,
                   * but the compiler can't know that */
}