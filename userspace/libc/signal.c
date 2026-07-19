#include "signal.h"
#include "syscall.h"
#include "stdlib.h"
#include <stddef.h>

#define MAX_SIGNALS 32

static sighandler_t g_handlers[MAX_SIGNALS];

sighandler_t signal(int signum, sighandler_t handler)
{
    if (signum < 0 || signum >= MAX_SIGNALS) return SIG_ERR;
    sighandler_t prev = g_handlers[signum];
    if (prev == NULL) prev = SIG_DFL; /* default until set otherwise */
    g_handlers[signum] = handler;

    /* Tell the kernel too, so a hardware fault can actually redirect
     * to this handler (see kernel/arch/x86_64/idt.c). SIG_DFL/SIG_IGN
     * aren't real function addresses, so register 0 (no kernel-level
     * handler) for those — default/kill behavior is what the kernel
     * already does when nothing's registered. */
    uint64_t kernel_handler =
        (handler == SIG_DFL || handler == SIG_IGN) ? 0 : (uint64_t)(uintptr_t)handler;
    sigaction_raw(signum, kernel_handler);

    return prev;
}

void sigreturn(void)
{
    sigreturn_raw();

    /* Only reached if the kernel refused the resume (no handler was
     * actually in progress). There is no faulting context to go back
     * to, so exit instead of returning into undefined state. */
    exit(-1);
}