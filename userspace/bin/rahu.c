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
#define MAX_PKG_SIZE   (256 * 1024)  /* 256KB max package size */

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
    /* Build URL: http://REGISTRY_HOST/index.json */
    const char *parts[] = { "http://", REGISTRY_HOST, "/index.json", (const char*)0 };
    int pos = 0;
    for (int i = 0; parts[i]; i++) {
        const char *s = parts[i];
        while (*s && pos < 255) url[pos++] = *s++;
    }
    url[pos] = 0;

    int64_t n = http_get(url, index_buf, sizeof(index_buf) - 1);
    if (n <= 0) {
        print("[rahu] Failed to fetch index. Error: ");
        print_uint((uint64_t)(-n));
        putc('\n');
        println("[rahu] Note: registry IP must be set in rahu.c");
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

static void cmd_install(const char *pkg)
{
    if (!pkg || !*pkg) { println("[rahu] Usage: rahu install <package>"); return; }

    print("[rahu] Installing: "); println(pkg);

    /* Static buffers to avoid stack overflow */
    static char url[256];
    static char install_path[64];

    /* Build download URL: http://host:port/packages/pkg.elf */
    int pos = 0;
    const char *base = "http://";
    const char *host = REGISTRY_HOST;
    const char *colon = ":";
    const char *port_str = "9090";
    const char *path = "/packages/";
    for (const char *s=base;     *s&&pos<255;) url[pos++]=*s++;
    for (const char *s=host;     *s&&pos<255;) url[pos++]=*s++;
    for (const char *s=colon;    *s&&pos<255;) url[pos++]=*s++;
    for (const char *s=port_str; *s&&pos<255;) url[pos++]=*s++;
    for (const char *s=path;     *s&&pos<255;) url[pos++]=*s++;
    for (const char *s=pkg;      *s&&pos<255;) url[pos++]=*s++;
    const char *ext = ".elf";
    for (const char *s=ext; *s&&pos<255;) url[pos++]=*s++;
    url[pos] = 0;

    print("[rahu] Downloading: "); println(url);

    /* Use mmap for download buffer — avoids large BSS */
    uint8_t *pkg_buf = (uint8_t *)mmap(MAX_PKG_SIZE);
    if (!pkg_buf) { println("[rahu] Out of memory."); return; }

    int64_t n = http_get(url, pkg_buf, MAX_PKG_SIZE);
    if (n <= 0) {
        print("[rahu] Download failed. Error: ");
        print_uint((uint64_t)(-n));
        putc('\n');
        munmap(pkg_buf, MAX_PKG_SIZE);
        return;
    }

    print("[rahu] Downloaded "); print_uint((uint64_t)n); println(" bytes.");

    /* Install to /bin/<name> */
    int ip = 0;
    const char *bindir = "/bin/";
    for (const char *s=bindir; *s&&ip<63;) install_path[ip++]=*s++;
    for (const char *s=pkg; *s&&ip<63;) install_path[ip++]=*s++;
    install_path[ip] = 0;

    int64_t w = file_write(install_path, pkg_buf, (uint64_t)n);
    munmap(pkg_buf, MAX_PKG_SIZE);

    if (w < 0) {
        print("[rahu] Failed to install to "); println(install_path);
        return;
    }

    print("[rahu] Installed: "); println(install_path);
    println("[rahu] Done. Run it with: spawn(\"");
    print(install_path);
    println("\")");
}

static void cmd_remove(const char *pkg)
{
    if (!pkg || !*pkg) { println("[rahu] Usage: rahu remove <package>"); return; }
    /* TODO: vfs_unlink syscall needed */
    print("[rahu] Remove not yet implemented for: "); println(pkg);
}

static void cmd_list(void)
{
    println("[rahu] Installed packages in /bin:");
    println("  (use 'ls /bin' in exploish to see installed binaries)");
}

/*  Entry point  */
void main(int argc, char **argv)
{
    /* Raw syscall write to confirm we reached main */
    const char msg[] = "RAHU ALIVE\n";
    write(1, msg, 11);
    puts("rahu: main entered\n");
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
    else if (str_eq(cmd, "help"))    cmd_help();
    else {
        print("[rahu] Unknown command: "); println(cmd);
        println("Run 'rahu help' for usage.");
    }
    /* explicit exit syscall */
    syscall1(SYS_EXIT, 0);
    for(;;) {}
}