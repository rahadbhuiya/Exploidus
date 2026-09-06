#include <stdio.h>
#include <stdlib.h>

/*
 * gidtest — verifies process_t.gid / exfs_inode_t.group_gid / the
 * three-tier (owner/group/other) permission check added this session
 * actually work end-to-end. Modeled on chmodtest.c's structure.
 *
 * setgid() only works while still root (see sys_setgid()'s comment),
 * and this shell's interactive prompt has already dropped to
 * UID_DEFAULT_USER by the time a command line could run something --
 * so running `gidtest` from the interactive shell always fails every
 * setgid()-dependent check (inherits the shell's already-dropped
 * uid=1000). To actually verify this, spawn it from somewhere that's
 * still root: temporarily add a `start_daemon("gidtest",
 * "/bin/gidtest")` + `waitpid()` call in userspace/bin/init.c, right
 * after the auditd/httpd start_daemon() calls (before the shell is
 * spawned) -- init itself never calls setuid(), so anything it spawns
 * starts at UID_ROOT. Remove that block again once done; this isn't
 * meant to run on every boot in a normal build (see git history for
 * the exact snippet if needed again).
 *
 * There's no fork() on this kernel, so both the "same group ->
 * allowed" and "different group -> denied" cases are exercised
 * within this one process by creating two files under two different
 * gids *before* the one-way setuid() drop, then checking access to
 * both *after* dropping -- rather than needing two separate
 * processes with two different starting gids.
 */

static void check(const char *label, int ok)
{
    printf("[%s] %s\n", ok ? " OK " : "FAIL", label);
}

int main(void)
{
    printf("gidtest: starting\n");

    check("starts as root (uid 0)", getuid() == UID_ROOT);
    check("starts with root group (gid 0)", getgid() == GID_ROOT);

    /* File 1: created under the default (root) group, group-only
     * access (mode 0060 -- owner and other both denied). */
    const char *path_rootgroup = "/tmp_gidtest_rootgroup.txt";
    FILE *f1 = fopen(path_rootgroup, "w");
    check("create file under root group", f1 != NULL);
    if (f1) { fprintf(f1, "root-group\n"); fclose(f1); }
    check("chmod(0060) on root-group file",
          chmod(path_rootgroup, 0060) == 0);

    /* Switch to a distinct group (still root -- allowed) and create
     * a second file, so it's owned by group 777 instead. */
    int sg = setgid(777);
    check("setgid(777) succeeds while still root", sg == 0);
    check("getgid() reflects the change", getgid() == 777);

    const char *path_mygroup = "/tmp_gidtest_mygroup.txt";
    FILE *f2 = fopen(path_mygroup, "w");
    check("create file under group 777", f2 != NULL);
    if (f2) { fprintf(f2, "group-777\n"); fclose(f2); }
    check("chmod(0060) on group-777 file",
          chmod(path_mygroup, 0060) == 0);

    /* Drop root -- from here on, uid=1000 (not the owner of either
     * file, which are both owned by uid 0), gid stays 777 (setuid
     * doesn't touch gid). This is the one-way step: setgid() can
     * never succeed again in this process after this line. */
    int su = setuid(UID_DEFAULT_USER);
    check("setuid() drop succeeds", su == 0);
    check("gid is unchanged by setuid()", getgid() == 777);

    /* Group 777 == this file's group -> group bits (rw-) should
     * grant access even though we're neither root nor the owner. */
    FILE *fa = fopen(path_mygroup, "r+");
    check("access GRANTED via matching group (777 == 777)", fa != NULL);
    if (fa) fclose(fa);

    /* Group 0 != our current group (777) -> falls through to
     * "other" bits, which mode 0060 leaves at 0 -> denied. */
    FILE *fb = fopen(path_rootgroup, "r+");
    check("access DENIED via non-matching group (777 != 0)", fb == NULL);
    if (fb) fclose(fb); /* shouldn't happen, but clean up if it does */

    /* Bonus: confirm the privilege-drop ordering rule itself --
     * setgid() must now refuse, since we're no longer root. */
    int sg2 = setgid(0);
    check("setgid() now refused post-setuid() (privilege-drop order enforced)",
          sg2 != 0);

    printf("gidtest: done\n");

    /* Clean up so a repeated run (e.g. every boot, if this stays
     * wired into init.c's startup sequence) doesn't hit the
     * duplicate-name guard on the second run. */
    unlink(path_rootgroup);
    unlink(path_mygroup);

    return 0;
}