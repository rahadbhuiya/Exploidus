#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

/*
 * sigtest — verifies real signal delivery (kernel/arch/x86_64/idt.c)
 * actually works: registers a SIGSEGV handler, then deliberately
 * dereferences an invalid pointer. If the handler runs and exits
 * cleanly, delivery works. If instead you see the OS's own
 * "[EXPLOIDUS] Killing faulting process" message with no "Caught
 * signal" line first, the handler wasn't invoked — signal delivery
 * isn't working and this is worth investigating.
 */

void crash_handler(int sig)
{
    printf("Caught signal %d! Handler is working.\n", sig);
    printf("Exiting cleanly from the handler (see the idt.c comment: \n");
    printf("handlers here must exit() themselves, not return).\n");
    exit(42);
}

int main(void)
{
    printf("sigtest: registering SIGSEGV handler...\n");
    signal(SIGSEGV, crash_handler);

    printf("sigtest: about to dereference an invalid pointer on purpose...\n");

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    volatile int *bad_ptr = (volatile int *)0x1;
    *bad_ptr = 42; /* deliberate crash: should trigger SIGSEGV */
#pragma GCC diagnostic pop

    printf("sigtest: if you see this, the crash didn't happen — bug in the test.\n");
    return 1;
}