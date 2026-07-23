#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

/*
 * sigtest -- verifies real signal delivery (kernel/arch/x86_64/idt.c),
 * resume (sys_sigreturn in kernel/syscall/table.c), and cross-process
 * kill() (sys_kill, same file) all work.
 */

/*  Test 1: cross-process kill()  */

static void term_handler(int sig)
{
    printf("child: caught signal %d, exiting cleanly\n", sig);
    exit(77); /* distinctive code so the parent can confirm the handler ran */
}

static void test_kill(void)
{
    printf("test_kill: forking...\n");
    int64_t pid = fork();

    if (pid == 0) {
        /* child */
        printf("child: PID=%lld, registering SIGTERM handler, waiting to be killed...\n",
               (long long)getpid());
        signal(SIGTERM, term_handler);
        for (int i = 0; i < 200; i++) {
            sleep_ticks(10);
        }
        printf("child: never got signaled after waiting, exiting normally\n");
        exit(1);
    }

    if (pid < 0) {
        printf("test_kill: fork failed\n");
        return;
    }

    /* parent */
    printf("parent: child PID=%lld, giving it a moment to register the handler\n",
           (long long)pid);
    sleep_ticks(20);

    printf("parent: sending SIGTERM to PID=%lld\n", (long long)pid);
    int64_t rc = kill_raw((int)pid, SIGTERM);
    printf("parent: kill_raw() returned %lld\n", (long long)rc);

    int64_t status = waitpid(pid);
    printf("parent: child exited with status %lld (77 = handler ran and caught it, "
           "1 = timed out and never got signaled, -1 = no such child)\n",
           (long long)status);
}

/*  Test 2: signal resume via sigreturn()  */

/*
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

static void test_resume(void)
{
    printf("sigtest: registering SIGSEGV handler...\n");
    signal(SIGSEGV, crash_handler);

    printf("sigtest: about to dereference an invalid pointer on purpose...\n");

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    *bad_ptr = 42; /* deliberate crash: should trigger SIGSEGV */
#pragma GCC diagnostic pop

    printf("sigtest: if you see this, something is wrong, this line should be unreachable.\n");
}

int main(void)
{
    printf("=== Test 1: cross-process kill() ===\n");
    test_kill();

    printf("\n=== Test 2: signal resume (sigreturn) ===\n");
    test_resume(); /* exits from inside crash_handler, does not return */

    return 1;
}