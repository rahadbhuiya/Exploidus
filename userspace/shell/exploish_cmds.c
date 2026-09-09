/*
 * exploish_cmds.c - extra shell commands
 */
#include "../libc/syscall.h"

static void _print(const char *s) { puts(s); }
static void _println(const char *s) { puts(s); putc('\n'); }
static void _print_int(int64_t n) {
    if (n < 0) { putc('-'); n = -n; }
    if (n == 0) { putc('0'); return; }
    char buf[20]; int i = 0;
    while (n > 0) { buf[i++] = (char)('0' + n % 10); n /= 10; }
    for (int j = i-1; j >= 0; j--) putc(buf[j]);
}
static const char *_skip(const char *s) { while (*s == ' ') s++; return s; }
static int _atoi(const char *s) {
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    int n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return n * sign;
}

static void _abs(const char *path, char *out, int max) {
    char tmp[256];
    int i = 0;

    /* Build full path */
    if (path[0] == '/') {
        /* absolute */
        while (*path && i < 255) tmp[i++] = *path++;
        tmp[i] = 0;
    } else {
        /* relative — get cwd first */
        if (getcwd(tmp, 256) < 0 || tmp[0] == '\0') {
            tmp[0] = '/'; tmp[1] = '\0';
        }
        i = 0;
        while (tmp[i]) i++;
        if (i > 1 && tmp[i-1] != '/') tmp[i++] = '/';
        while (*path && i < 255) tmp[i++] = *path++;
        tmp[i] = 0;
    }

    /* Resolve . and .. */
    char res[256];
    int  ri = 0;
    char *p = tmp;
    res[ri++] = '/';

    while (*p) {
        /* skip slashes */
        while (*p == '/') p++;
        if (!*p) break;

        /* read component */
        char comp[128];
        int ci = 0;
        while (*p && *p != '/') comp[ci++] = *p++;
        comp[ci] = 0;

        if (ci == 0) continue;
        if (ci == 1 && comp[0] == '.') continue; /* . */
        if (ci == 2 && comp[0] == '.' && comp[1] == '.') {
            /* go up — remove last component */
            while (ri > 1 && res[ri-1] != '/') ri--;
            if (ri > 1) ri--; /* remove trailing slash */
            continue;
        }

        /* append component */
        if (ri > 1) res[ri++] = '/';
        int j = 0;
        while (comp[j] && ri < max-1) res[ri++] = comp[j++];
    }

    res[ri] = 0;
    if (ri == 0) { res[0] = '/'; res[1] = 0; }

    i = 0;
    while (res[i] && i < max-1) out[i] = res[i], i++;
    out[i] = 0;
}

void cmd_ext_cd(const char *path) {
    if (!*path) path = "/";
    char ap[256];
    _abs(path, ap, 256);
    if (chdir(ap) < 0) { _print("cd: no such directory: "); _println(ap); }
}

void cmd_ext_pwd(void) {
    char buf[256];
    if (getcwd(buf, 256) == 0) _println(buf);
    else _println("/");
}

void cmd_ext_whoami(void) { _println("root"); }

void cmd_ext_hostname(void) { _println("exploidus"); }

void cmd_ext_mkdir(const char *path) {
    if (!*path) { _println("Usage: mkdir <dir>"); return; }
    char ap[256]; _abs(path, ap, 256);
    int fd = fs_create(ap, 1);
    if (fd < 0) { _print("mkdir: failed: "); _println(ap); return; }
    close(fd);
    _print("created: "); _println(ap);
}

void cmd_ext_rmdir(const char *path) {
    if (!*path) { _println("Usage: rmdir <dir>"); return; }
    char ap[256]; _abs(path, ap, 256);
    int r = rmdir(ap);
    if (r == 0) { _print("removed: "); _println(ap); return; }
    if (r == -2) { _println("rmdir: directory not empty"); return; }
    if (r == -3) { _println("rmdir: not a directory"); return; }
    _print("rmdir: failed: "); _println(ap);
}

void cmd_ext_rm(const char *path) {
    if (!*path) { _println("Usage: rm <file>"); return; }
    char ap[256]; _abs(path, ap, 256);
    int r = unlink(ap);
    if (r != 0) { puts("rm: failed: "); _println(ap); }
}

/*
 * crashtest <1|2|3|4> -- TEST ONLY. Arms a deterministic crash inside
 * the next ExFS journal commit (see SYS_DEBUG_EXFS_CRASH), then
 * triggers the filesystem operation that hits it. The VM halts
 * (triple-faults) right there -- QEMU should reset. On reboot, check
 * the boot log for "[ExFS] Journal: replaying..." (point 2 should
 * show it; points 1 and 3 should NOT), then `ls /crashtest_probe` to
 * confirm the filesystem is in a sane state either way.
 *
 * Points 1-3 test create()'s journal commit (file absent for point 1,
 * present for points 2 and 3 -- never a corrupted/partial entry).
 *
 * Point 4 is different: it tests exfs_op_unlink()'s block-freeing
 * reordering (see the comment in exfs_op_unlink() in exfs.c) --
 * crashes right after the dirent-removal+inode-zeroing transaction
 * commits, before the file's now-unreferenced blocks are freed.
 * Expect: /crashtest_probe is gone (the delete itself was durable),
 * but `python3 tools/fsck.py build/disk.img` should report a leaked
 * block -- that's the correct, acceptable outcome (a leak, fixable
 * with `--fix`), not corruption.
 */
void cmd_ext_crashtest(const char *args)
{
    const char *p = _skip(args);
    if (!*p || (*p < '1' || *p > '4') ||
        (p[1] != '\0' && p[1] != ' ')) {
        _println("Usage: crashtest <1|2|3|4>");
        _println("  1 = crash before journal commit point (expect: no replay, file absent)");
        _println("  2 = crash after commit, before real apply (expect: replay runs, file present)");
        _println("  3 = crash after real apply, before journal clear (expect: no replay needed, file present)");
        _println("  4 = crash after unlink's commit, before its blocks are freed");
        _println("      (expect: file absent; fsck.py should report a leaked block, not corruption)");
        return;
    }
    int point = *p - '0';
    _println("Arming crash test -- the VM will halt in a moment.");

    if (point == 4) {
        /* Point 4 needs a file that actually has at least one
         * allocated data block, so unlink() has something to free
         * (and therefore something to potentially leak if the crash
         * lands where this test aims it). fs_create() alone makes an
         * empty file with no blocks at all -- deleting that can't
         * demonstrate anything about the block-freeing reorder, since
         * there'd be nothing to free either way. Write a byte to
         * force one direct-block allocation first (unarmed). If the
         * file already exists from an earlier run, both calls below
         * just fail harmlessly and the pre-existing one (which does
         * have a block, from its own earlier write) is used instead. */
        int fd = fs_create("/crashtest_probe", 0);
        if (fd >= 0) {
            write(fd, "x", 1);
            close(fd);
        }
        _println("After it resets, run:  python3 tools/fsck.py build/disk.img");
        _println("(expect exactly one leaked block, no errors), and:");
        _println("  ls /crashtest_probe   (expect: gone)");
        debug_exfs_crash(point);
        unlink("/crashtest_probe"); /* never returns once armed */
        return;
    }

    _println("After it resets, check the boot log and run:");
    _println("  ls /crashtest_probe");
    debug_exfs_crash(point);
    /* This create() is what triggers the armed journal commit -- and
     * with the crash points inside it, this call never returns. */
    int fd = fs_create("/crashtest_probe", 0);
    if (fd >= 0) close(fd); /* only reached if point was out of range */
}

/*
 * mkmanyfiles <dir> <count> -- TEST ONLY. Creates <count> empty files
 * named f0, f1, ... f<count-1> inside <dir>, entirely in-kernel/in-
 * process (no external interpreter, no scripting) -- built to test
 * ExFS directory growth past EXFS_MAX_DIR_BLOCKS' old 12-direct-block
 * cap (~180 entries) without depending on Lua (which has an unrelated
 * crash on this kernel unconnected to any of this -- same fault
 * address regardless of script content, so it's something in Lua's
 * own ELF-load/startup path, not a filesystem issue). Prints progress
 * every 25 files so a hang is visible rather than a silent wait, and
 * stops with a clear message on the first failure instead of
 * plowing through further errors.
 */
void cmd_ext_mkmanyfiles(const char *args)
{
    const char *p = _skip(args);
    char dir[240];
    int i = 0;
    while (*p && *p != ' ' && i < 239) dir[i++] = *p++;
    dir[i] = 0;
    p = _skip(p);
    if (!dir[0] || !*p) {
        _println("Usage: mkmanyfiles <dir> <count>");
        return;
    }
    int count = 0;
    while (*p >= '0' && *p <= '9') { count = count * 10 + (*p - '0'); p++; }
    if (count <= 0) { _println("mkmanyfiles: count must be a positive number"); return; }

    char adir[256]; _abs(dir, adir, 256);
    int adir_len = 0;
    while (adir[adir_len]) adir_len++;

    for (int n = 0; n < count; n++) {
        char path[300];
        int k = 0;
        while (adir[k] && k < 250) { path[k] = adir[k]; k++; }
        if (k == 0 || path[k-1] != '/') path[k++] = '/';
        path[k++] = 'f';
        /* itoa(n) */
        char numbuf[12]; int ni = 0;
        int nn = n;
        if (nn == 0) numbuf[ni++] = '0';
        while (nn > 0 && ni < 11) { numbuf[ni++] = (char)('0' + nn % 10); nn /= 10; }
        while (ni > 0 && k < 299) path[k++] = numbuf[--ni];
        path[k] = 0;

        int fd = fs_create(path, 0);
        if (fd < 0) {
            puts("mkmanyfiles: failed at file #"); 
            /* simple positive-int print since there's no itoa-print
             * helper visible in this file */
            {
                char nb[12]; int j = 0; int v = n;
                if (v == 0) nb[j++] = '0';
                while (v > 0 && j < 11) { nb[j++] = (char)('0' + v % 10); v /= 10; }
                while (j > 0) { char c[2] = { nb[--j], 0 }; puts(c); }
            }
            puts(" ("); puts(path); _println(")");
            return;
        }
        close(fd);

        if ((n + 1) % 25 == 0) {
            char nb[12]; int j = 0; int v = n + 1;
            if (v == 0) nb[j++] = '0';
            while (v > 0 && j < 11) { nb[j++] = (char)('0' + v % 10); v /= 10; }
            puts("  ");
            while (j > 0) { char c[2] = { nb[--j], 0 }; puts(c); }
            _println(" files created so far...");
        }
    }
    _println("mkmanyfiles: done");
}

/*
 * ln <target> <linkpath> -- create a symlink named <linkpath> whose
 * content is the literal string <target> (not validated or resolved
 * at creation time -- a dangling or, currently, relative target is
 * not an error here; relative targets just won't be followable, see
 * the comment in vfs_lookup_impl() in vfs.c).
 */
void cmd_ext_ln(const char *args)
{
    const char *p = _skip(args);
    char target[256], linkpath[256];
    int i = 0;
    while (*p && *p != ' ' && i < 255) target[i++] = *p++;
    target[i] = 0;
    p = _skip(p);
    i = 0;
    while (*p && i < 255) linkpath[i++] = *p++;
    linkpath[i] = 0;
    if (!target[0] || !linkpath[0]) {
        _println("Usage: ln <target> <linkpath>  (creates a symlink)");
        return;
    }
    char alink[256]; _abs(linkpath, alink, 256);
    /* target is stored as-is, NOT made absolute -- symlink(2) never
     * rewrites its target argument either; an absolute target (e.g.
     * "/bin/lua") is what vfs_lookup_impl() can currently follow,
     * a relative one is stored fine but won't resolve when followed. */
    if (symlink(target, alink) != 0) {
        puts("ln: failed: "); puts(target); puts(" -> "); _println(alink);
    }
}

/* readlink <path> -- print a symlink's stored target verbatim. */
void cmd_ext_readlink(const char *path)
{
    const char *p = _skip(path);
    if (!*p) { _println("Usage: readlink <path>"); return; }
    char ap[256]; _abs(p, ap, 256);
    char buf[256];
    int64_t n = readlink(ap, buf, sizeof(buf) - 1);
    if (n < 0) { puts("readlink: not a symlink or not found: "); _println(ap); return; }
    buf[n] = 0;
    _println(buf);
}

/*
 * setgid <n> -- see sys_setgid()'s comment (kernel/syscall/table.c)
 * for the exact rule: only works while still root, so this must be
 * run before anything drops privilege with setuid (this shell itself
 * calls setuid(UID_DEFAULT_USER) once, right before showing the
 * first prompt -- so setgid only works from a script/command run
 * before that point, or from a freshly spawned process that hasn't
 * dropped yet).
 */
void cmd_ext_setgid(const char *args)
{
    const char *p = _skip(args);
    if (!*p) { _println("Usage: setgid <n>"); return; }
    int n = 0;
    while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
    if (setgid((uint32_t)n) != 0)
        _println("setgid: failed (must still be root -- see 'help')");
}

/* chgrp <gid> <path> */
void cmd_ext_chgrp(const char *args)
{
    const char *p = _skip(args);
    if (!*p) { _println("Usage: chgrp <gid> <path>"); return; }
    int gid = 0;
    while (*p >= '0' && *p <= '9') { gid = gid * 10 + (*p - '0'); p++; }
    p = _skip(p);
    if (!*p) { _println("Usage: chgrp <gid> <path>"); return; }
    char ap[256]; _abs(p, ap, 256);
    if (chgrp(ap, (uint32_t)gid) != 0) {
        puts("chgrp: failed: "); _println(ap);
    }
}

/*
 * corrupttest -- TEST ONLY. Creates a file, writes real content to it
 * (so it gets a real, non-zero whole-file BLAKE3 hash -- see
 * exfs_op_write() in exfs.c), corrupts its first data block directly
 * on disk (bypassing the normal write path entirely, so the stored
 * hash is left stale -- simulates genuine silent corruption, not a
 * legitimate write), then opens it again. Prints what happened;
 * the actual pass/fail signal is the
 * "[ExFS] INTEGRITY WARNING: ... possible silent corruption" line
 * this should print to the boot/serial log during that final open --
 * check for it there, this command doesn't have a way to read the
 * kernel's own serial output back.
 */
void cmd_ext_corrupttest(const char *args)
{
    (void)args;
    const char *path = "/corrupttest_probe";

    int fd = fs_create(path, 0);
    if (fd < 0) {
        _println("corrupttest: create failed (leftover from a previous "
                 "run? try 'rm /corrupttest_probe' first)");
        return;
    }
    write(fd, "corruption test payload, twenty-plus bytes so it's not trivially short", 40);
    close(fd);
    _println("corrupttest: file created and written (hash now set)");

    if (debug_exfs_corrupt(path) != 0) {
        _println("corrupttest: corruption injection failed");
        return;
    }
    _println("corrupttest: corrupted on disk (bypassing normal write)");

    _println("corrupttest: re-opening -- check the serial/boot log now for:");
    _println("  [ExFS] INTEGRITY WARNING: ... possible silent corruption");
    int fd2 = open(path, O_RDONLY);
    if (fd2 >= 0) close(fd2);

    unlink(path);
    _println("corrupttest: done, probe file removed");
}

void cmd_ext_mount(const char *args)
{
    const char *p = _skip(args);
    if (!*p) {
        _println("mount: usage: mount <device> <mountpoint>");
        _println("  e.g.: mount usb0 /media/usb0");
        return;
    }

    char dev_name[32];
    int i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(dev_name) - 1) {
        dev_name[i++] = *p++;
    }
    dev_name[i] = '\0';

    p = _skip(p);
    if (!*p) { _println("mount: missing mountpoint"); return; }

    char mountpoint[256];
    _abs(p, mountpoint, sizeof(mountpoint));

    int r = mount(dev_name, mountpoint);
    if (r == 0) {
        _print("mounted "); _print(dev_name);
        _print(" at "); _println(mountpoint);
        return;
    }
    switch (r) {
        case -2: _println("mount: no such device (check the name -- "
                          "e.g. \"usb0\")"); break;
        case -3: _println("mount: not a valid ExFS volume on that "
                          "device (unformatted stick?)"); break;
        case -4: _println("mount: mount table full"); break;
        default: _println("mount: failed"); break;
    }
}

void cmd_ext_mkfs(const char *args)
{
    const char *p = _skip(args);
    if (!*p) {
        _println("mkfs: usage: mkfs <device> <blocks>");
        _println("  e.g.: mkfs usb0 2000   (2000 * 4096 bytes = ~8MB)");
        _println("  WARNING: destroys any existing data on the device.");
        _println("  <blocks> must fit within the device's real capacity");
        _println("  -- check the READ CAPACITY line in the boot log.");
        return;
    }

    char dev_name[32];
    int i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(dev_name) - 1) {
        dev_name[i++] = *p++;
    }
    dev_name[i] = '\0';

    p = _skip(p);
    if (!*p) { _println("mkfs: missing block count"); return; }

    int blocks = _atoi(p);
    if (blocks <= 0) { _println("mkfs: invalid block count"); return; }

    int r = mkfs(dev_name, (uint64_t)blocks);
    if (r == 0) {
        _print("formatted "); _print(dev_name);
        _println(" with a fresh ExFS volume");
        return;
    }
    switch (r) {
        case -2: _println("mkfs: no such device"); break;
        case -3: _println("mkfs: format failed (block count too small, "
                          "or a write to the device failed)"); break;
        default: _println("mkfs: failed"); break;
    }
}

void cmd_ext_free(void) {
    _println("              total        used        free");
    _println("Mem:          262144      --          --");
}

void cmd_ext_uptime(void) {
    uint64_t secs = uptime();
    uint64_t hh = secs/3600, mm = (secs%3600)/60, ss = secs%60;
    _print("up ");
    _print_int((int64_t)hh); _print("h ");
    _print_int((int64_t)mm); _print("m ");
    _print_int((int64_t)ss); _println("s");
}

void cmd_ext_kill(const char *args) {
    if (!*args) { _println("Usage: kill <pid> [signal]"); return; }
    int64_t pid = 0;
    while (*args >= '0' && *args <= '9') pid = pid*10 + (*args++ - '0');

    args = _skip(args);
    int64_t sig = 15; /* default: SIGTERM-equivalent */
    if (*args) {
        sig = 0;
        while (*args >= '0' && *args <= '9') sig = sig*10 + (*args++ - '0');
    }

    if (kill_raw((int)pid, (int)sig) != 0) {
        _print("kill: no such process: "); _print_int(pid); _println("");
        return;
    }
    _print("kill: sent signal "); _print_int(sig);
    _print(" to PID "); _print_int(pid); _println("");
}

void cmd_ext_ifconfig(void) {
    _println("lo        Link encap:Local Loopback");
    _println("          inet addr:127.0.0.1");
    _println("");
    _println("eth0      Link encap:Ethernet");
    _println("          inet addr:10.0.2.15");
    _println("          Mask:255.255.255.0");
}

void cmd_ext_wc(const char *path) {
    if (!*path) { _println("Usage: wc <file>"); return; }
    char ap[256]; _abs(path, ap, 256);
    int fd = open(ap, 0);
    if (fd < 0) { _print("wc: not found: "); _println(ap); return; }
    char buf[512]; int64_t n;
    int64_t lines=0, words=0, bytes=0, in_word=0;
    while ((n = read(fd, buf, 511)) > 0) {
        bytes += n;
        for (int64_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == 10) lines++;
            if (c == 32 || c == 9 || c == 10) { in_word = 0; }
            else if (!in_word) { in_word = 1; words++; }
        }
    }
    close(fd);
    _print_int(lines); putc(9);
    _print_int(words); putc(9);
    _print_int(bytes); putc(9);
    _println(path);
}

void cmd_ext_grep(const char *args) {
    const char *p = args;
    while (*p && *p != ' ') p++;
    if (!*p) { _println("Usage: grep <pattern> <file>"); return; }
    char pat[128];
    int plen = (int)(p - args);
    if (plen > 127) plen = 127;
    for (int i = 0; i < plen; i++) pat[i] = args[i];
    pat[plen] = 0;
    const char *fpath = _skip(p);
    char ap[256]; _abs(fpath, ap, 256);
    int fd = open(ap, 0);
    if (fd < 0) { _print("grep: not found: "); _println(ap); return; }
    char line[512]; int li = 0;
    char ch[1]; int64_t n;
    while ((n = read(fd, ch, 1)) > 0) {
        if (ch[0] == 10 || li >= 511) {
            line[li] = 0;
            int found = 0;
            for (int i = 0; line[i] && !found; i++) {
                int match = 1;
                for (int j = 0; pat[j] && match; j++)
                    if (line[i+j] != pat[j]) match = 0;
                if (match) found = 1;
            }
            if (found) _println(line);
            li = 0;
        } else { line[li++] = ch[0]; }
    }
    close(fd);
}

void cmd_ext_head(const char *args) {
    int lines = 10;
    const char *path = args;
    if (args[0] == '-' && args[1] == 'n') {
        path = _skip(args + 2);
        lines = 0;
        while (*path >= '0' && *path <= '9') lines = lines*10 + (*path++ - '0');
        path = _skip(path);
    }
    if (!*path) { _println("Usage: head [-n num] <file>"); return; }
    char ap[256]; _abs(path, ap, 256);
    int fd = open(ap, 0);
    if (fd < 0) { _print("head: not found: "); _println(ap); return; }
    char buf[512]; int64_t n; int count = 0;
    while (count < lines && (n = read(fd, buf, 511)) > 0) {
        for (int64_t i = 0; i < n && count < lines; i++) {
            putc(buf[i]);
            if (buf[i] == 10) count++;
        }
    }
    close(fd);
}

void cmd_ext_xxd(const char *path) {
    if (!*path) { _println("Usage: xxd <file>"); return; }
    char ap[256]; _abs(path, ap, 256);
    int fd = open(ap, 0);
    if (fd < 0) { _print("xxd: not found: "); _println(ap); return; }
    uint8_t buf[16]; int64_t n; uint64_t offset = 0;
    while ((n = read(fd, buf, 16)) > 0) {
        char hx[9]; uint64_t v = offset;
        for (int i = 7; i >= 0; i--) {
            uint8_t nb = (uint8_t)(v & 0xF);
            hx[i] = (char)(nb < 10 ? '0'+nb : 'a'+nb-10);
            v >>= 4;
        }
        hx[8] = 0;
        puts(hx); puts(": ");
        for (int64_t i = 0; i < n; i++) {
            uint8_t hi = (buf[i] >> 4) & 0xF;
            uint8_t lo = buf[i] & 0xF;
            putc((char)(hi < 10 ? '0'+hi : 'a'+hi-10));
            putc((char)(lo < 10 ? '0'+lo : 'a'+lo-10));
            putc(' ');
            if (i == 7) putc(' ');
        }
        puts(" |");
        for (int64_t i = 0; i < n; i++)
            putc(buf[i] >= 32 && buf[i] < 127 ? (char)buf[i] : '.');
        _println("|");
        offset += (uint64_t)n;
    }
    close(fd);
}
/*
 *  New Unix-style commands
 *  All use only Exploidus syscall wrappers -- no Linux libc dependency.
 * ==========================================================================
 */

/*  cat  */
void cmd_ext_cat(const char *path)
{
    if (!path || !*path) { _println("cat: missing file"); return; }
    char ap[256]; _abs(path, ap, 256);
    int fd = open(ap, O_RDONLY);
    if (fd < 0) { puts("cat: cannot open: "); _println(ap); return; }
    char buf[512];
    int64_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(STDOUT_FILENO, buf, (size_t)n);
    close(fd);
}

/*  touch  */
void cmd_ext_touch(const char *path)
{
    if (!path || !*path) { _println("touch: missing file"); return; }
    char ap[256]; _abs(path, ap, 256);
    int fd = open(ap, O_RDONLY);
    if (fd >= 0) { close(fd); return; }           /* already exists */
    fd = open(ap, O_WRONLY | O_CREAT);
    if (fd >= 0) close(fd);
    else { puts("touch: cannot create: "); _println(ap); }
}

/*  cp  */
void cmd_ext_cp(const char *args)
{
    /* cp <src> <dst> */
    const char *p = _skip(args);
    char src[256], dst[256];
    int i = 0;
    while (*p && *p != ' ' && i < 255) src[i++] = *p++;
    src[i] = 0;
    p = _skip(p);
    i = 0;
    while (*p && i < 255) dst[i++] = *p++;
    dst[i] = 0;
    if (!src[0] || !dst[0]) { _println("cp: usage: cp <src> <dst>"); return; }

    char asrc[256], adst[256];
    _abs(src, asrc, 256); _abs(dst, adst, 256);

    int fdin = open(asrc, O_RDONLY);
    if (fdin < 0) { puts("cp: cannot open: "); _println(asrc); return; }

    int fdout = open(adst, O_WRONLY | O_CREAT);
    if (fdout < 0) { close(fdin); puts("cp: cannot create: "); _println(adst); return; }

    char buf[512]; int64_t n;
    while ((n = read(fdin, buf, sizeof(buf))) > 0)
        write(fdout, buf, (size_t)n);
    close(fdin); close(fdout);
}

/*  mv  */
void cmd_ext_mv(const char *args)
{
    const char *p = _skip(args);
    char src[256], dst[256];
    int i = 0;
    while (*p && *p != ' ' && i < 255) src[i++] = *p++;
    src[i] = 0;
    p = _skip(p);
    i = 0;
    while (*p && i < 255) dst[i++] = *p++;
    dst[i] = 0;
    if (!src[0] || !dst[0]) { _println("mv: usage: mv <src> <dst>"); return; }

    char asrc[256], adst[256];
    _abs(src, asrc, 256); _abs(dst, adst, 256);

    /* "mv <src> <dir>/" convenience: a destination ending in '/' means
     * "into this directory, keeping the source's own name" -- same as
     * real mv(1). Checked on the *raw, pre-_abs()* dst: _abs() is a
     * general path normalizer that always strips a trailing slash
     * while resolving the path (same as any "/dir2/" -> "/dir2"
     * normalization), so by the time `adst` exists the trailing slash
     * this check needs is already gone -- checking `adst` here (an
     * earlier version of this did exactly that) means the check can
     * never fire. The rename() syscall itself still requires the
     * exact destination path (real rename(2) works the same way;
     * this basename-append is a shell-level convenience, not a
     * kernel feature), so it's resolved here before calling it.
     */
    int dst_raw_len = 0;
    while (dst[dst_raw_len]) dst_raw_len++;
    if (dst_raw_len > 0 && dst[dst_raw_len - 1] == '/') {
        int adst_len = 0;
        while (adst[adst_len]) adst_len++;
        /* find src's basename (text after its last '/') */
        int last_slash = -1;
        for (int j = 0; asrc[j]; j++) if (asrc[j] == '/') last_slash = j;
        const char *base = (last_slash >= 0) ? asrc + last_slash + 1 : asrc;
        /* adst is "/dir2" (root) or ".../dir2" (nested) -- append a
         * '/' separator unless adst is bare "/" (root itself, where
         * appending would double the slash). */
        int k = adst_len;
        if (!(adst_len == 1 && adst[0] == '/')) adst[k++] = '/';
        while (*base && k < 255) adst[k++] = *base++;
        adst[k] = 0;
    }

    /* Used to be implemented as cp-then-unlink (a full read+write copy
     * followed by removing the source). Now that ExFS supports a real
     * rename(), use that instead: it's a single atomic (journaled)
     * directory-entry move rather than a byte-for-byte copy, so it's
     * both faster (no data ever moves) and safe against a crash
     * midway leaving both a partial destination and a deleted
     * source -- the old cp+unlink approach had exactly that failure
     * window. */
    if (__sys_rename(asrc, adst) != 0) {
        puts("mv: failed: "); puts(asrc); puts(" -> "); _println(adst);
    }
}

/*  tail  */
/* Simple tail: reads up to 4KB from end of file, prints last N lines    */
void cmd_ext_tail(const char *args)
{
    int nlines = 10;
    const char *p = _skip(args);

    /* parse optional -N */
    if (*p == '-') {
        p++;
        nlines = 0;
        while (*p >= '0' && *p <= '9') nlines = nlines * 10 + (*p++ - '0');
        p = _skip(p);
    }
    if (!*p) { _println("tail: missing file"); return; }

    char ap[256]; _abs(p, ap, 256);
    int fd = open(ap, O_RDONLY);
    if (fd < 0) { puts("tail: cannot open: "); _println(ap); return; }

    /* Read last 4KB */
    static char tbuf[4096];
    int64_t total = 0, n;
    while ((n = read(fd, tbuf + total,
                     (size_t)(4096 - total))) > 0) {
        total += n;
        if (total >= 4096) break;
    }
    close(fd);
    if (total == 0) return;

    /* Count newlines from end */
    int found = 0;
    int64_t start = total;
    for (int64_t i = total - 1; i >= 0 && found < nlines; i--) {
        if (tbuf[i] == '\n') { found++; start = i + 1; }
    }
    if (found < nlines) start = 0;
    write(STDOUT_FILENO, tbuf + start, (size_t)(total - start));
}

/*  find  */
/* find <dir> [-name <pattern>]  — simple recursive listing              */
static int _str_match(const char *pat, const char *str)
{
    /* Very minimal glob: only * wildcard */
    while (*pat && *str) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            while (*str) {
                if (_str_match(pat, str)) return 1;
                str++;
            }
            return 0;
        }
        if (*pat != *str) return 0;
        pat++; str++;
    }
    while (*pat == '*') pat++;
    return !*pat && !*str;
}

static void _find_dir(const char *dir, const char *pat, int depth)
{
    if (depth > 8) return;
    char abuf[256]; _abs(dir, abuf, 256);
    int fd = open(abuf, O_RDONLY);
    if (fd < 0) return;

    dirent_t ents[32]; int64_t n;
    while ((n = readdir(fd, ents, 32)) > 0) {
        for (int64_t i = 0; i < n; i++) {
            if (ents[i].name[0] == '.' &&
                (ents[i].name[1] == 0 ||
                 (ents[i].name[1] == '.' && ents[i].name[2] == 0))) continue;

            char full[256];
            int ai = 0;
            const char *d = abuf;
            while (*d && ai < 250) full[ai++] = *d++;
            if (ai > 1 && full[ai-1] != '/') full[ai++] = '/';
            const char *nm = ents[i].name;
            while (*nm && ai < 255) full[ai++] = *nm++;
            full[ai] = 0;

            if (!pat || _str_match(pat, ents[i].name)) {
                _println(full);
            }
            if (ents[i].type == 1)   /* directory */
                _find_dir(full, pat, depth + 1);
        }
    }
    close(fd);
}

void cmd_ext_find(const char *args)
{
    const char *p = _skip(args);
    char dir[256] = ".";
    const char *pat = (const char *)0;
    int i = 0;

    /* parse dir */
    if (*p && *p != '-') {
        while (*p && *p != ' ' && i < 255) dir[i++] = *p++;
        dir[i] = 0;
        p = _skip(p);
    }

    /* parse -name <pat> */
    if (p[0] == '-' && p[1] == 'n' && p[2] == 'a' &&
        p[3] == 'm' && p[4] == 'e') {
        p = _skip(p + 5);
        pat = p;
    }

    _find_dir(dir, (*pat ? pat : (const char *)0), 0);
}

/*  sed  */
/* sed s/old/new/  <file>  — single substitution per line               */
void cmd_ext_sed(const char *args)
{
    const char *p = _skip(args);

    /* parse s/old/new/ */
    if (*p != 's') { _println("sed: only s/old/new/ supported"); return; }
    p++;
    char delim = *p++;
    char old[128], newstr[128];
    int oi = 0, ni = 0;
    while (*p && *p != delim && oi < 127) old[oi++] = *p++;
    old[oi] = 0;
    if (*p == delim) p++;
    while (*p && *p != delim && ni < 127) newstr[ni++] = *p++;
    newstr[ni] = 0;
    if (*p == delim) p++;
    if (*p == delim) p++;  /* trailing / or g */

    p = _skip(p);
    if (!*p) { _println("sed: missing file"); return; }

    char ap[256]; _abs(p, ap, 256);
    int fd = open(ap, O_RDONLY);
    if (fd < 0) { puts("sed: cannot open: "); _println(ap); return; }

    static char line[1024];
    int li = 0;
    char ch;

    while (read(fd, &ch, 1) == 1) {
        if (ch == '\n' || li >= 1023) {
            line[li] = 0;
            /* Search and replace in line */
            char out[1024]; int oo = 0;
            char *lp = line;
            while (*lp) {
                /* try match */
                int matched = 1;
                for (int k = 0; k < oi; k++) {
                    if (lp[k] != old[k]) { matched = 0; break; }
                }
                if (matched && oi > 0) {
                    for (int k = 0; k < ni && oo < 1023; k++) out[oo++] = newstr[k];
                    lp += oi;
                } else {
                    if (oo < 1023) out[oo++] = *lp++;
                }
            }
            out[oo] = 0;
            puts(out); putc('\n');
            li = 0;
        } else {
            line[li++] = ch;
        }
    }
    /* flush last line without newline */
    if (li > 0) {
        line[li] = 0;
        _println(line);
    }
    close(fd);
}

/*  chmod  */
/* ExFS doesn't have Unix permissions yet — stub that confirms the call  */
void cmd_ext_chmod(const char *args)
{
    /* Just acknowledge — ExFS permission model is capability-based */
    const char *p = _skip(args);
    if (!*p) { _println("chmod: usage: chmod <mode> <file>"); return; }
    /* skip mode */
    while (*p && *p != ' ') p++;
    p = _skip(p);
    if (!*p) { _println("chmod: missing file"); return; }
    /* ExFS has no Unix rwx bits — silently succeed like busybox on tmpfs */
    (void)p;
}

/*  env  */
void cmd_ext_env(void)
{
    /* Exploidus has no env vars yet — print minimal POSIX-like set */
    _println("PATH=/bin:/usr/bin");
    _println("HOME=/");
    _println("SHELL=/bin/exploish");
    _println("TERM=vt100");
    _println("USER=root");
}

/*  clear  */
void cmd_ext_clear(void)
{
    /* ANSI clear screen + cursor home */
    puts("\033[2J\033[H");
}

/*  tee  */
/* tee <file>  — reads stdin, writes to file AND stdout                  */
void cmd_ext_tee(const char *path)
{
    if (!path || !*path) { _println("tee: missing file"); return; }
    char ap[256]; _abs(path, ap, 256);

    int fd = open(ap, O_WRONLY | O_CREAT);
    if (fd < 0) { puts("tee: cannot open: "); _println(ap); return; }

    char buf[512]; int64_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, (size_t)n);
        write(fd, buf, (size_t)n);
    }
    close(fd);
}

/*  cmp  */
/* cmp <file1> <file2>  — byte-by-byte comparison                        */
void cmd_ext_cmp(const char *args)
{
    const char *p = _skip(args);
    char f1[256], f2[256];
    int i = 0;
    while (*p && *p != ' ' && i < 255) f1[i++] = *p++;
    f1[i] = 0; p = _skip(p); i = 0;
    while (*p && i < 255) f2[i++] = *p++;
    f2[i] = 0;
    if (!f1[0] || !f2[0]) { _println("cmp: usage: cmp <file1> <file2>"); return; }

    char a1[256], a2[256];
    _abs(f1, a1, 256); _abs(f2, a2, 256);

    int fd1 = open(a1, O_RDONLY), fd2 = open(a2, O_RDONLY);
    if (fd1 < 0) { puts("cmp: cannot open: "); _println(a1); return; }
    if (fd2 < 0) { close(fd1); puts("cmp: cannot open: "); _println(a2); return; }

    char b1, b2; int64_t pos = 1; int same = 1;
    while (1) {
        int64_t r1 = read(fd1, &b1, 1);
        int64_t r2 = read(fd2, &b2, 1);
        if (r1 <= 0 && r2 <= 0) break;
        if (r1 <= 0 || r2 <= 0 || b1 != b2) { same = 0; break; }
        pos++;
    }
    close(fd1); close(fd2);

    if (same) _println("files are identical");
    else {
        puts("files differ at byte ");
        _print_int(pos);
        putc('\n');
    }
}