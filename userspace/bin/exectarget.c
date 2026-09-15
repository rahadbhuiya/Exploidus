/*
 * Only include stdio.h -- it pulls in syscall.h itself (execve(),
 * getpid(), write(), ...). Including "../libc/syscall.h" directly
 * *before* stdio.h would redeclare puts()/putc() with a conflicting
 * signature (stdio.h defines _STDIO_H_ before its own #include
 * "syscall.h", which is what makes syscall.h skip its raw-syscall
 * puts()/putc() shims in favor of stdio.h's real ones -- see
 * syscall.h's "#ifndef _STDIO_H_" around them).
 */
#include <stdio.h>

/*
 * exectarget — the program exectest.c execve()'s into. Never run
 * directly on its own for the interesting part of the test (it's a
 * normal, spawn()-able binary too, it just won't have anything
 * interesting in argv/envp if you do). Prints:
 *
 *   - its own pid (compare against the pid the shell reported when
 *     it *spawned exectest* -- if they match, execve() really did
 *     replace the process in place rather than starting a new one)
 *   - every argv[] it received (proves argv passed through execve())
 *   - every envp[] it received (proves envp passed through execve())
 *   - a decrementing "hop count" in argv[1], if present, and whether
 *     it should execve() again -- see exectest.c's ping-pong mode
 */

int main(int argc, char **argv, const char **envp)
{
    printf("exectarget: my pid=%lld\n", (long long)getpid());

    printf("exectarget: argc=%d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("exectarget: argv[%d]=%s\n", i, argv[i]);

    int envc = 0;
    if (envp) { while (envp[envc]) envc++; }
    printf("exectarget: envc=%d\n", envc);
    for (int i = 0; i < envc; i++)
        printf("exectarget: envp[%d]=%s\n", i, envp[i]);

    /* Ping-pong mode: argv[1] is a decimal hop counter. If it's still
     * above zero, execve() back into ourselves with it decremented --
     * this repeatedly exercises the "free the old address space"
     * path in sys_execve_impl() (kernel/proc/fork_exec.c) many times
     * in a row under the SAME pid, which a single one-shot exec can't
     * catch (a leak on every call only shows up after several). Run
     * `free` in the shell before and after to compare physical-page
     * counts. */
    if (argc >= 2) {
        int hops = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
            hops = hops * 10 + (*p - '0');

        if (hops > 0) {
            char nbuf[16];
            int n = hops - 1, i = 0;
            if (n == 0) { nbuf[i++] = '0'; }
            char tmp[16]; int t = 0;
            while (n > 0) { tmp[t++] = (char)('0' + (n % 10)); n /= 10; }
            while (t > 0) nbuf[i++] = tmp[--t];
            nbuf[i] = 0;

            printf("exectarget: %d hop(s) left, execve() again\n", hops);
            const char *nargv[] = { "/bin/exectarget", nbuf, (const char *)0 };
            int64_t r = execve("/bin/exectarget", nargv, envp);
            printf("exectarget: execve() FAILED, returned %lld\n", (long long)r);
            return 1;
        }
    }

    printf("exectarget: done, exiting normally\n");
    return 42;
}