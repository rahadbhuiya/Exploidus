/* Only include stdio.h -- see the comment in exectarget.c for why not
 * to also include "../libc/syscall.h" directly here. */
#include <stdio.h>

/*
 * exectest — end-to-end test for SYS_EXECVE / sys_execve_impl()
 * (kernel/proc/fork_exec.c). Run it from the shell like any other
 * program:
 *
 *   exectest
 *
 * The shell prints the pid it spawned exectest as. Watch for
 * "exectarget: my pid=" in the output right after -- it must be the
 * SAME pid. That's the actual thing that distinguishes real execve()
 * (replaces the calling process) from spawn()/SYS_EXECV (starts a
 * new, separate process): if the pids differ, execve() is behaving
 * like spawn() and something is wrong.
 *
 * Passes argv = ["/bin/exectarget", "3", (custom args)] and one
 * envp entry -- exectarget.c prints everything it receives, and
 * ping-pongs execve() back into itself 3 more times (hop count),
 * which exercises the old-address-space-free path repeatedly under
 * one pid instead of just once. Run the shell's `free` command
 * before running this and again after it finishes to compare
 * physical-page counts -- they should end up the same (mod whatever
 * the shell/system itself allocated meanwhile), not trending downward
 * run after run.
 */

int main(void)
{
    int64_t pid_before = getpid();
    printf("exectest: my pid (before execve)=%lld\n", (long long)pid_before);
    printf("exectest: calling execve(\"/bin/exectarget\", ...)\n");

    const char *argv[] = {
        "/bin/exectarget", "3", "hello", "world", (const char *)0
    };
    const char *envp[] = {
        "EXECTEST_VAR=exploidus_execve_works", (const char *)0
    };

    int64_t r = execve("/bin/exectarget", argv, envp);

    /* Only reached if execve() FAILED -- on success this process's
     * own image (this code, this stack) no longer exists, and
     * control never comes back here. */
    printf("exectest: execve() FAILED, returned %lld\n", (long long)r);
    printf("exectest: -2 = /bin/exectarget not found on disk, "
           "-1 = ELF load or OOM failure\n");
    return 1;
}