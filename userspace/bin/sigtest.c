#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

/*
 * sigtest -- verifies real signal delivery (kernel/arch/x86_64/idt.c)
 * and resume (sys_sigreturn in kernel/syscall/table.c) both work.
 *
 * Registers a SIGSEGV handler, then deliberately dereferences an
 * invalid pointer. The handler resumes the faulting instruction with
 * sigreturn() a few times in a row before finally exiting, so this
 * also proves resume is stable across repeated round trips and not
 * just a one-shot trick. If the handler never runs at all, you will
 * see the OS's own "[EXPLOIDUS] Killing faulting process" message
 * with no "Caught signal" line first, and signal delivery itself is
 * broken, not just resume.
 */

static volatile int *bad_ptr = (volatile int *)0x1;
static int hits = 0;

void crash_handler(int sig)
{
    hits++;
    printf("Caught signal %d, hit #%d\n", sig, hits);

    if (hits >= 3) {
        printf("Handler is working and resume is stable. Exiting cleanly.\n");
        exit(42);
    }

    printf("Resuming with sigreturn() (this will fault again at the same instruction on purpose).\n");
    sigreturn();
}

int main(void)
{
    printf("sigtest: registering SIGSEGV handler...\n");
    signal(SIGSEGV, crash_handler);

    printf("sigtest: about to dereference an invalid pointer on purpose...\n");

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    *bad_ptr = 42; /* deliberate crash: should trigger SIGSEGV */
#pragma GCC diagnostic pop

    printf("sigtest: if you see this, something is wrong, this line should be unreachable.\n");
    return 1;
}