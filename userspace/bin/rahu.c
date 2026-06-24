/*
 * rahu — Exploidus Package Manager
 *
 * Usage:
 *   rahu install <package>
 *   rahu remove  <package>
 *   rahu list
 *   rahu search  <query>
 *   rahu update
 *
 * Downloads static ELF binaries from the Exploidus package registry.
 * Packages are installed to /bin/<name>
 */

#include "../libc/syscall.h"

/*  Registry  */
#define REGISTRY_HOST  "10.0.2.2"
#define REGISTRY_PORT  9090
#define REGISTRY_PATH  "/packages/"

/*  Helpers  */
static void print(const char *s) { puts(s); }
static void println(const char *s) { puts(s); putc('\n'); }
static void print_uint(uint64_t n)
{
    if (!n) { putc('0'); return; }
    char buf[21]; int i=0;
    while(n){ buf[i++]='0'+(int)(n%10); n/=10; }
    while(i--) putc(buf[i]);
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* case-sensitive substring search, like strstr */
static int str_contains(const char *hay, const char *needle)
{
    if (!*needle) return 1;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* Print each non-empty line of a text file, optionally filtered to
 * lines containing `filter` (pass NULL or "" to print every line).
 * Returns the number of lines printed, or -1 if the file doesn't exist. */
static int print_matching_lines(const char *path, const char *filter)
{
    int fd = open(path, 0);
    if (fd < 0) return -1;

    char line[256]; int li = 0;
    char ch[1]; int64_t n;
    int count = 0;

    while ((n = read(fd, ch, 1)) > 0) {
        if (ch[0] == '\n' || li >= 255) {
            line[li] = 0;
            if (li > 0 && (!filter || !*filter || str_contains(line, filter))) {
                print("  "); println(line);
                count++;
            }
            li = 0;
        } else {
            line[li++] = ch[0];
        }
    }
    if (li > 0) {
        line[li] = 0;
        if (!filter || !*filter || str_contains(line, filter)) {
            print("  "); println(line);
            count++;
        }
    }
    close(fd);
    return count;
}

/* Encode `len` raw bytes as lowercase hex into out (must hold len*2+1). */
static void hex_encode(const uint8_t *in, int len, char *out)
{
    const char *hexchars = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        out[i*2]   = hexchars[(in[i] >> 4) & 0xF];
        out[i*2+1] = hexchars[in[i] & 0xF];
    }
    out[len*2] = 0;
}

/*  Commands  */

static void cmd_help(void)
{
    println("Rahu — Exploidus Package Manager");
    println("");
    println("Usage:");
    println("  rahu install <package>   Install a package");
    println("  rahu remove  <package>   Remove a package");
    println("  rahu list                List installed packages");
    println("  rahu search  <query>     Search available packages");
    println("  rahu update              Update package index");
    println("  rahu help                Show this help");
    println("");
    println("Examples:");
    println("  rahu install nginx");
    println("  rahu install sqlite");
    println("  rahu install busybox");
}

static void cmd_update(void)
{
    println("[rahu] Fetching package index...");

    static uint8_t index_buf[65536];
    static char url[256];
    /* Build URL: http://REGISTRY_HOST:9090/index.json */
    const char *parts[] = { "http://", REGISTRY_HOST, ":9090/index.json", (const char*)0 };
    int pos = 0;
    for (int i = 0; parts[i]; i++) {
        const char *s = parts[i];
        while (*s && pos < 255) url[pos++] = *s++;
    }
    url[pos] = 0;

    int64_t n = http_get(url, index_buf, sizeof(index_buf) - 1);
    if (n <= 0) {
        if (n == -8) {
            println("[rahu] Registry returned an error (no index.json there?).");
        } else {
            print("[rahu] Failed to fetch index. Error: ");
            print_uint((uint64_t)(-n));
            putc('\n');
            println("[rahu] Note: registry IP must be set in rahu.c");
        }
        return;
    }
    index_buf[n] = 0;

    /* Write index to /var/rahu/index.json */
    int64_t w = file_write("/var/rahu/index.json", index_buf, (uint64_t)n);
    if (w < 0) {
        println("[rahu] Failed to save index.");
        return;
    }
    print("[rahu] Index updated. ");
    print_uint((uint64_t)n);
    println(" bytes.");
}

static void cmd_search(const char *query)
{
    if (!query || !*query) { println("[rahu] Usage: rahu search <query>"); return; }

    print("[rahu] Searching for: "); println(query);

    /* Index is plain text, one package entry per line (e.g. "name - desc").
     * There is no JSON parser in this libc, so this is a line-based
     * substring scan rather than real JSON parsing. */
    int matches = print_matching_lines("/var/rahu/index.json", query);
    if (matches < 0) {
        println("[rahu] No local package index. Run 'rahu update' first.");
        return;
    }
    if (matches == 0) println("[rahu] No matches. Try 'rahu update' to refresh the index.");
}

/* Looks up "<pkg> <64-char-hex-hash> ..." in /var/rahu/index.json.
 * Returns 1 and fills hash_out[65] (null-terminated lowercase hex) if a
 * full 64-char hash is found for an exact package-name match, else 0. */
static int index_lookup_hash(const char *pkg, char *hash_out)
{
    int fd = open("/var/rahu/index.json", 0);
    if (fd < 0) return 0;

    char line[256]; int li = 0;
    char ch[1]; int64_t n;
    int found = 0;

    while (!found && (n = read(fd, ch, 1)) > 0) {
        if (ch[0] == '\n' || li >= 255) {
            line[li] = 0;
            const char *p = line; const char *q = pkg;
            while (*q && *p == *q) { p++; q++; }
            if (!*q && *p == ' ') {
                p++;
                int hi = 0;
                while (*p && *p != ' ' && hi < 64) hash_out[hi++] = *p++;
                hash_out[hi] = 0;
                if (hi == 64) found = 1;
            }
            li = 0;
        } else {
            line[li++] = ch[0];
        }
    }
    close(fd);
    return found;
}

/* Best-effort install history for 'rahu list'. file_write always
 * overwrites (there is no append syscall), so read what's there,
 * add a line in memory, and write the whole thing back. */
static void write_manifest_entry(const char *pkg, uint64_t size)
{
    static uint8_t buf[16384];
    int64_t existing = 0;

    int fd = open("/var/rahu/installed.json", 0);
    if (fd >= 0) {
        existing = read(fd, buf, sizeof(buf) - 256);
        if (existing < 0) existing = 0;
        close(fd);
    }

    int64_t pos = existing;
    for (const char *s = pkg; *s && pos < (int64_t)sizeof(buf) - 32; s++) buf[pos++] = (uint8_t)*s;
    buf[pos++] = ' ';

    char numbuf[21]; int ni = 0;
    uint64_t v = size;
    if (!v) numbuf[ni++] = '0';
    while (v) { numbuf[ni++] = '0' + (int)(v % 10); v /= 10; }
    while (ni--) buf[pos++] = (uint8_t)numbuf[ni];
    buf[pos++] = '\n';

    file_write("/var/rahu/installed.json", buf, (uint64_t)pos);
}

static void cmd_install(const char *pkg)
{
    if (!pkg || !*pkg) { println("[rahu] Usage: rahu install <package>"); return; }

    print("[rahu] Installing: "); println(pkg);

    static char url[256];
    static char install_path[64];

    /* Build install path: /bin/<name> */
    int ip = 0;
    for (const char *s = "/bin/"; *s && ip < 63;) install_path[ip++] = *s++;
    for (const char *s = pkg;    *s && ip < 63;) install_path[ip++] = *s++;
    install_path[ip] = 0;

    /* Build download URL: http://host:port/packages/pkg.elf */
    int pos = 0;
    for (const char *s = "http://";          *s && pos < 255;) url[pos++] = *s++;
    for (const char *s = REGISTRY_HOST;      *s && pos < 255;) url[pos++] = *s++;
    for (const char *s = ":9090/packages/";  *s && pos < 255;) url[pos++] = *s++;
    for (const char *s = pkg;                *s && pos < 255;) url[pos++] = *s++;
    for (const char *s = ".elf";             *s && pos < 255;) url[pos++] = *s++;
    url[pos] = 0;

    print("[rahu] Downloading: "); println(url);

    /* Look up an expected hash before downloading, so we know whether
     * to even ask the kernel to hash it. */
    char expected_hex[65];
    int  have_expected = index_lookup_hash(pkg, expected_hex);

    /* Stream directly to /bin/<name> — no userspace buffer, no size cap.
     * digest[32] is a tiny stack buffer; the kernel does the actual
     * file-sized hashing work on its own heap. */
    uint8_t digest[32];
    int64_t n = http_download(url, install_path, have_expected ? digest : (uint8_t*)0);
    if (n <= 0) {
        if (n == -8) println("[rahu] Package not found on registry (HTTP error).");
        else { print("[rahu] Download failed. Error: "); print_uint((uint64_t)(-n)); putc('\n'); }
        return;
    }

    print("[rahu] Downloaded "); print_uint((uint64_t)n); println(" bytes.");

    /* Best-effort BLAKE3 integrity check against /var/rahu/index.json.
     * No entry on record → install proceeds with a warning.
     * Confirmed mismatch → refuse and remove.
     *
     * NOTE: the kernel BLAKE3 is single-chunk-only above 1024 bytes
     * (see kernel/crypto/blake3.c), so for larger files this hash will
     * differ from standard BLAKE3 tools — that's expected. Use
     * tools/gen_index.py on the server side; it replicates the same
     * quirk so the hashes line up. */
    if (have_expected) {
        char actual_hex[65];
        hex_encode(digest, 32, actual_hex);
        if (!str_eq(actual_hex, expected_hex)) {
            println("[rahu] BLAKE3 hash mismatch — removing and refusing to install.");
            print("[rahu]   expected: "); println(expected_hex);
            print("[rahu]   got:      "); println(actual_hex);
            unlink(install_path);
            return;
        }
        println("[rahu] BLAKE3 hash verified.");
    } else {
        println("[rahu] No hash on record — installing unverified. Run 'rahu update' first.");
    }

    write_manifest_entry(pkg, (uint64_t)n);
    print("[rahu] Installed: "); println(install_path);
    print("[rahu] Done. Run with: "); println(install_path);
}

/* Remove `pkg`'s line from /var/rahu/installed.json, if present.
 * Same "no append syscall" constraint as write_manifest_entry — read
 * it all in, rebuild without the matching line, write it all back. */
static void remove_manifest_entry(const char *pkg)
{
    int fd = open("/var/rahu/installed.json", 0);
    if (fd < 0) return;  /* no manifest yet, nothing to do */

    static uint8_t in_buf[16384];
    int64_t in_len = read(fd, in_buf, sizeof(in_buf) - 1);
    close(fd);
    if (in_len < 0) return;
    in_buf[in_len] = 0;

    static uint8_t out_buf[16384];
    int64_t out_pos = 0;
    int64_t line_start = 0;

    for (int64_t i = 0; i <= in_len; i++) {
        if (i < in_len && in_buf[i] != '\n') continue;

        int64_t line_len = i - line_start;
        if (line_len > 0) {
            const char *line = (const char *)in_buf + line_start;
            const char *q = pkg;
            int64_t k = 0;
            while (q[k] && k < line_len && line[k] == q[k]) k++;
            int is_match = (!q[k] && (k == line_len || line[k] == ' '));

            if (!is_match) {
                for (int64_t j = 0; j < line_len; j++) out_buf[out_pos++] = (uint8_t)line[j];
                out_buf[out_pos++] = '\n';
            }
        }
        line_start = i + 1;
    }

    file_write("/var/rahu/installed.json", out_buf, (uint64_t)out_pos);
}

static void cmd_remove(const char *pkg)
{
    if (!pkg || !*pkg) { println("[rahu] Usage: rahu remove <package>"); return; }

    static char path[64];
    int ip = 0;
    const char *bindir = "/bin/";
    for (const char *s = bindir; *s && ip < 63;) path[ip++] = *s++;
    for (const char *s = pkg;    *s && ip < 63;) path[ip++] = *s++;
    path[ip] = 0;

    int r = unlink(path);
    if (r < 0) {
        print("[rahu] Could not remove "); print(path);
        println(r == -2 ? " (it's a directory)" : " (not found)");
        return;
    }

    remove_manifest_entry(pkg);
    print("[rahu] Removed: "); println(path);
}

static void cmd_list(void)
{
    println("[rahu] Binaries in /bin:");

    int fd = open("/bin", 0);
    if (fd < 0) {
        println("  (could not open /bin)");
    } else {
        dirent_t entries[64];
        int64_t count = readdir(fd, entries, 64);
        close(fd);
        if (count <= 0) {
            println("  (empty)");
        } else {
            for (int64_t i = 0; i < count; i++) {
                const char *nm = entries[i].name;
                if (*nm && (unsigned char)*nm < 32) nm++;  /* skip stray ctrl byte */
                print("  "); println(nm);
            }
        }
    }

    println("");
    println("[rahu] Install history (/var/rahu/installed.json):");
    int hist = print_matching_lines("/var/rahu/installed.json", (const char *)0);
    if (hist < 0)  println("  (none yet — try 'rahu install <pkg>')");
    if (hist == 0) println("  (none yet)");
}

/*  Entry point  */
void main(int argc, char **argv)
{
    if (argc < 2) { cmd_help(); return; }

    /* argv[1] may be "install hello" as a single string — split it */
    char cmd_buf[32]; char arg_buf[64];
    cmd_buf[0] = 0; arg_buf[0] = 0;
    const char *s = argv[1];
    int i = 0;
    while (*s && *s != ' ' && i < 31) cmd_buf[i++] = *s++;
    cmd_buf[i] = 0;
    while (*s == ' ') s++;
    i = 0;
    while (*s && i < 63) arg_buf[i++] = *s++;
    arg_buf[i] = 0;

    const char *cmd = cmd_buf;
    const char *arg = arg_buf[0] ? arg_buf : (const char*)0;

    if      (str_eq(cmd, "install")) cmd_install(arg);
    else if (str_eq(cmd, "remove"))  cmd_remove(arg);
    else if (str_eq(cmd, "list"))    cmd_list();
    else if (str_eq(cmd, "update"))  cmd_update();
    else if (str_eq(cmd, "search"))  cmd_search(arg);
    else if (str_eq(cmd, "help"))    cmd_help();
    else {
        print("[rahu] Unknown command: "); println(cmd);
        println("Run 'rahu help' for usage.");
    }
    /* explicit exit syscall */
    syscall1(SYS_EXIT, 0);
    for(;;) {}
}