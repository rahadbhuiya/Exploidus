/*
 * init.c — Exploidus Init System v0.3.0
 *
 * Default: headless server mode (text console).
 * GUI: user runs `alien` from the shell to launch compositor.
 */

#include "../libc/syscall.h"

static void print(const char *s)   { write(1, s, strlen(s)); }
static void println(const char *s) { print(s); write(1, "\n", 1); }
static void print_uint(uint64_t n)
{
    if (n == 0) { write(1, "0", 1); return; }
    char buf[21]; int i = 0;
    while (n) { buf[i++] = '0' + (int)(n % 10); n /= 10; }
    while (i--) { char c = buf[i]; write(1, &c, 1); }
}

static int64_t start_daemon(const char *name, const char *path)
{
    print("[INIT] Starting "); print(name); print(" ... ");
    int64_t pid = spawn(path);
    if (pid < 0) { println("FAILED"); return -1; }
    print("PID="); print_uint((uint64_t)pid); print("\n");
    return pid;
}

void main(void)
{
    println("");
    println("╔═══════════════════════════════════════════╗");
    println("║     Exploidus Init System  v0.3.0         ║");
    println("║     Type 'alien' for graphical mode     ║");
    println("╚═══════════════════════════════════════════╝");
    println("");
    println("[INIT] PID 1 — Exploidus Init");
    println("[INIT] Headless server mode (default)");
    println("[INIT] Starting system services...");
    println("");

    /* System daemons — always start */
    start_daemon("auditd", "/bin/auditd");
    start_daemon("httpd",  "/bin/httpd");

    println("");
    println("[INIT] All services started.");
    println("[INIT] Starting interactive shell...");
    println("[INIT] Tip: type 'alien' to launch GUI");
    println("");

    /* Interactive shell */
    int64_t shell_pid = spawn("/bin/exploish");
    if (shell_pid < 0) {
        println("[INIT] Fatal: could not start shell");
        reboot();
    }
    print("[INIT] Shell PID="); print_uint((uint64_t)shell_pid); print("\n");
    println("─────────────────────────────────────────────────");
    println("");

    waitpid(shell_pid);

    println("");
    println("[INIT] Session ended. Powering off...");
    sleep_ticks(30);
    poweroff();
}