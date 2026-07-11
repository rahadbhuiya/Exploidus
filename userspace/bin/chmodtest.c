#include <stdio.h>
#include <stdlib.h>

/*
 * chmodtest — verifies chmod() + the VFS permission enforcement added
 * this session actually work end-to-end:
 *   1. create a file, write to it (should work — default mode is
 *      writable)
 *   2. chmod it to 0444 (read-only)
 *   3. try to open it for writing — should now FAIL
 *   4. confirm it's still readable
 *   5. chmod it back to 0644 (writable) and confirm writing works
 *      again
 */

static void check(const char *label, int ok)
{
    printf("[%s] %s\n", ok ? " OK " : "FAIL", label);
}

int main(void)
{
    const char *path = "/tmp_chmodtest.txt";

    printf("chmodtest: starting\n");

    /* 1. Create + write (default mode should allow this) */
    FILE *f = fopen(path, "w");
    check("create + open for write", f != NULL);
    if (!f) { printf("chmodtest: can't continue without the file, aborting\n"); return 1; }
    fprintf(f, "hello\n");
    fclose(f);

    /* 2. Make it read-only */
    int cr = chmod(path, 0444);
    check("chmod(0444) succeeded", cr == 0);

    /* 3. Writing should now be refused */
    FILE *fw = fopen(path, "w");
    check("write blocked on read-only file", fw == NULL);
    if (fw) fclose(fw); /* shouldn't happen, but clean up if it does */

    /* 4. Reading should still work */
    FILE *fr = fopen(path, "r");
    check("read still allowed on read-only file", fr != NULL);
    if (fr) {
        char buf[32];
        char *got = fgets(buf, sizeof(buf), fr);
        check("read content matches what was written", got && buf[0] == 'h');
        fclose(fr);
    }

    /* 5. Restore write access and confirm it works again */
    int cr2 = chmod(path, 0644);
    check("chmod(0644) succeeded", cr2 == 0);

    FILE *fw2 = fopen(path, "w");
    check("write allowed again after chmod(0644)", fw2 != NULL);
    if (fw2) { fprintf(fw2, "updated\n"); fclose(fw2); }

    printf("chmodtest: done\n");
    return 0;
}