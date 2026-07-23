/*
 * rahu -- Exploidus Package Manager
 *
 * Usage:
 *   rahu install <package>
 *   rahu remove  <package>
 *   rahu list
 *   rahu search  <query>
 *   rahu update
 *
 * Downloads .fozu package archives from the Exploidus package
 * registry and unpacks them. A .fozu file can bundle more than one
 * installed file (a binary plus config files, etc) with a name and
 * version -- see tools/mkfozu.py for the exact container format and
 * unpack_fozu() below for the reader.
 *
 * Each installed package's file list is recorded at
 * /var/rahu/installed/<pkg>.list (one absolute path per line) so
 * `rahu remove` can clean up every file a package put down, not just
 * a single guessed /bin/<pkg> path.
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

/*  .fozu package format (see tools/mkfozu.py)  */

static int read_exact(int fd, void *buf, uint64_t n)
{
    uint8_t *p = (uint8_t *)buf;
    uint64_t got = 0;
    while (got < n) {
        int64_t r = read(fd, p + got, n - got);
        if (r <= 0) return -1;
        got += (uint64_t)r;
    }
    return 0;
}

static int read_u32(int fd, uint32_t *out) { return read_exact(fd, out, 4); }
static int read_u64(int fd, uint64_t *out) { return read_exact(fd, out, 8); }

static void ensure_dir(const char *path)
{
    int fd = fs_create(path, 1);
    if (fd >= 0) close(fd);
}

/* /var/rahu/{tmp,installed} need to exist before we can write into
 * them. fs_create() on an existing directory is expected to fail;
 * that is fine, we just ignore it. */
static void ensure_rahu_dirs(void)
{
    ensure_dir("/var");
    ensure_dir("/var/rahu");
    ensure_dir("/var/rahu/tmp");
    ensure_dir("/var/rahu/installed");
}

/* Appends `line` plus a newline to the file at `path`, creating it if
 * needed. Same "no append syscall" constraint as write_manifest_entry
 * below: read what's there, add the line, write it all back. Fine for
 * the small per-package manifest files this is used for. */
static void append_line(const char *path, const char *line)
{
    static uint8_t buf[16384];
    int64_t pos = 0;

    int fd = open(path, 0);
    if (fd >= 0) {
        pos = read(fd, buf, sizeof(buf) - 256);
        if (pos < 0) pos = 0;
        close(fd);
    }

    for (const char *s = line; *s && pos < (int64_t)sizeof(buf) - 2; s++)
        buf[pos++] = (uint8_t)*s;
    buf[pos++] = '\n';

    file_write(path, buf, (uint64_t)pos);
}

/* Copies `size` bytes from src_fd to a newly-created file at
 * install_path with the given mode, streaming through a fixed chunk
 * buffer instead of loading the whole file into memory. Returns 0 on
 * success, -1 on failure. */
static int extract_one_file(int src_fd, const char *install_path,
                             uint32_t mode, uint64_t size)
{
    int dst_fd = fs_create(install_path, 0);
    if (dst_fd < 0) {
        /* Likely already exists (reinstall/upgrade) -- replace it. */
        unlink(install_path);
        dst_fd = fs_create(install_path, 0);
        if (dst_fd < 0) return -1;
    }

    static uint8_t chunk[8192];
    uint64_t remaining = size;
    while (remaining > 0) {
        uint64_t want = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        if (read_exact(src_fd, chunk, want) < 0) { close(dst_fd); return -1; }
        int64_t w = write(dst_fd, chunk, want);
        if (w < 0 || (uint64_t)w != want) { close(dst_fd); return -1; }
        remaining -= want;
    }
    close(dst_fd);
    chmod(install_path, mode);
    return 0;
}

/* Unpacks a .fozu container already downloaded to fozu_path. Extracts
 * every contained file to its recorded install path and writes
 * /var/rahu/installed/<pkg>.list (one absolute path per line) so
 * `rahu remove` can find everything again later.
 * Returns the number of files installed, or -1 on error. */
static int unpack_fozu(const char *fozu_path, const char *pkg_name)
{
    int fd = open(fozu_path, 0);
    if (fd < 0) return -1;

    char magic[4];
    if (read_exact(fd, magic, 4) < 0 ||
        magic[0] != 'F' || magic[1] != 'O' || magic[2] != 'Z' || magic[3] != 'U') {
        println("[rahu]   not a valid .fozu file (bad magic).");
        close(fd);
        return -1;
    }

    uint32_t format_ver;
    if (read_u32(fd, &format_ver) < 0) { close(fd); return -1; }

    uint32_t name_len;
    if (read_u32(fd, &name_len) < 0 || name_len > 200) { close(fd); return -1; }
    char name_buf[201];
    if (read_exact(fd, name_buf, name_len) < 0) { close(fd); return -1; }
    name_buf[name_len] = 0;

    uint32_t ver_len;
    if (read_u32(fd, &ver_len) < 0 || ver_len > 64) { close(fd); return -1; }
    char ver_buf[65];
    if (read_exact(fd, ver_buf, ver_len) < 0) { close(fd); return -1; }
    ver_buf[ver_len] = 0;

    uint32_t file_count;
    if (read_u32(fd, &file_count) < 0) { close(fd); return -1; }

    print("[rahu]   package: "); print(name_buf);
    print(" v"); println(ver_buf);

    static char manifest_path[80];
    int mp = 0;
    for (const char *s = "/var/rahu/installed/"; *s && mp < 60;) manifest_path[mp++] = *s++;
    for (const char *s = pkg_name; *s && mp < 60;) manifest_path[mp++] = *s++;
    for (const char *s = ".list"; *s && mp < 79;) manifest_path[mp++] = *s++;
    manifest_path[mp] = 0;

    /* Fresh install list every time -- an upgrade might drop files
     * the previous version had. */
    unlink(manifest_path);

    int installed = 0;
    for (uint32_t i = 0; i < file_count; i++) {
        uint32_t path_len;
        if (read_u32(fd, &path_len) < 0 || path_len > 200) goto fail;
        char path_buf[201];
        if (read_exact(fd, path_buf, path_len) < 0) goto fail;
        path_buf[path_len] = 0;

        uint32_t mode;
        if (read_u32(fd, &mode) < 0) goto fail;

        uint64_t size;
        if (read_u64(fd, &size) < 0) goto fail;

        if (extract_one_file(fd, path_buf, mode, size) < 0) goto fail;

        print("[rahu]   installed "); println(path_buf);
        append_line(manifest_path, path_buf);
        installed++;
    }

    close(fd);
    return installed;

fail:
    println("[rahu]   package archive ended unexpectedly (truncated download?).");
    close(fd);
    return installed > 0 ? installed : -1;
}



/*  Commands  */

static void cmd_help(void)
{
    println("Rahu -- Exploidus Package Manager");
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

    ensure_rahu_dirs();

    print("[rahu] Installing: "); println(pkg);

    static char url[256];
    static char staging_path[80];

    /* Staging path: /var/rahu/tmp/<pkg>.fozu -- downloaded here first
     * so it can be hash-verified and unpacked, rather than streaming
     * straight to the final install path like the old single-.elf
     * scheme did (a package can contain more than one file now). */
    int sp = 0;
    for (const char *s = "/var/rahu/tmp/"; *s && sp < 60;) staging_path[sp++] = *s++;
    for (const char *s = pkg;              *s && sp < 60;) staging_path[sp++] = *s++;
    for (const char *s = ".fozu";          *s && sp < 79;) staging_path[sp++] = *s++;
    staging_path[sp] = 0;

    /* Build download URL: http://host:port/packages/pkg.fozu */
    int pos = 0;
    for (const char *s = "http://";          *s && pos < 255;) url[pos++] = *s++;
    for (const char *s = REGISTRY_HOST;      *s && pos < 255;) url[pos++] = *s++;
    for (const char *s = ":9090/packages/";  *s && pos < 255;) url[pos++] = *s++;
    for (const char *s = pkg;                *s && pos < 255;) url[pos++] = *s++;
    for (const char *s = ".fozu";             *s && pos < 255;) url[pos++] = *s++;
    url[pos] = 0;

    print("[rahu] Downloading: "); println(url);

    /* Look up an expected hash before downloading, so we know whether
     * to even ask the kernel to hash it. */
    char expected_hex[65];
    int  have_expected = index_lookup_hash(pkg, expected_hex);

    /* Stream directly to the staging path -- no userspace buffer, no
     * size cap. digest[32] is a tiny stack buffer; the kernel does
     * the actual file-sized hashing work on its own heap. */
    uint8_t digest[32];
    int64_t n = http_download(url, staging_path, have_expected ? digest : (uint8_t*)0);
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
     * differ from standard BLAKE3 tools -- that's expected. Use
     * tools/gen_index.py on the server side; it replicates the same
     * quirk so the hashes line up. */
    if (have_expected) {
        char actual_hex[65];
        hex_encode(digest, 32, actual_hex);
        if (!str_eq(actual_hex, expected_hex)) {
            println("[rahu] BLAKE3 hash mismatch -- removing and refusing to install.");
            print("[rahu]   expected: "); println(expected_hex);
            print("[rahu]   got:      "); println(actual_hex);
            unlink(staging_path);
            return;
        }
        println("[rahu] BLAKE3 hash verified.");
    } else {
        println("[rahu] No hash on record -- installing unverified. Run 'rahu update' first.");
    }

    int installed = unpack_fozu(staging_path, pkg);
    unlink(staging_path);

    if (installed <= 0) {
        println("[rahu] Failed to unpack package (not a valid .fozu archive?).");
        return;
    }

    write_manifest_entry(pkg, (uint64_t)n);
    print("[rahu] Installed "); print_uint((uint64_t)installed); println(" file(s).");
}

/* Remove `pkg`'s line from /var/rahu/installed.json, if present.
 * Same "no append syscall" constraint as write_manifest_entry -- read
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

    static char manifest_path[80];
    int mp = 0;
    for (const char *s = "/var/rahu/installed/"; *s && mp < 60;) manifest_path[mp++] = *s++;
    for (const char *s = pkg;                    *s && mp < 60;) manifest_path[mp++] = *s++;
    for (const char *s = ".list";                *s && mp < 79;) manifest_path[mp++] = *s++;
    manifest_path[mp] = 0;

    int fd = open(manifest_path, 0);
    if (fd < 0) {
        print("[rahu] No installed-file record for "); print(pkg); println(".");
        println("[rahu] (installed before rahu supported .fozu, or already removed?)");
        return;
    }

    char line[256]; int li = 0; char ch[1]; int64_t n; int removed = 0;
    while ((n = read(fd, ch, 1)) > 0) {
        if (ch[0] == '\n' || li >= 255) {
            line[li] = 0;
            if (li > 0) {
                int r = unlink(line);
                if (r == 0) { print("[rahu]   removed "); println(line); removed++; }
                else        { print("[rahu]   could not remove "); println(line); }
            }
            li = 0;
        } else {
            line[li++] = ch[0];
        }
    }
    close(fd);

    unlink(manifest_path);
    remove_manifest_entry(pkg);

    print("[rahu] Removed "); print_uint((uint64_t)removed); println(" file(s).");
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
    if (hist < 0)  println("  (none yet -- try 'rahu install <pkg>')");
    if (hist == 0) println("  (none yet)");
}

/*  Entry point  */
void main(int argc, char **argv)
{
    if (argc < 2) { cmd_help(); return; }

    /* argv[1] may be "install hello" as a single string -- split it */
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