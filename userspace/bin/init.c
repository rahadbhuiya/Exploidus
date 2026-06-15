/*
 * init.c — Exploidus Init System
 * PID 1. Spawns all system daemons, then monitors them.
 */

#include "../libc/syscall.h"

static void print(const char *s) { puts(s); }
static void println(const char *s) { puts(s); putc('\n'); }
static void print_uint(uint64_t n)
{
    if (n == 0) { putc('0'); return; }
    char buf[21]; int i = 0;
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) putc(buf[i]);
}

static int64_t start_daemon(const char *name, const char *path)
{
    print("[INIT] Starting ");
    print(name);
    print(" ... ");
    int64_t pid = spawn(path);
    if (pid < 0) { println("FAILED"); return -1; }
    print("PID=");
    print_uint((uint64_t)pid);
    putc('\n');
    return pid;
}

void main(void)
{
    println("");
    println("╔═══════════════════════════════════════════╗");
    println("║     Exploidus Init System  v0.1.0         ║");
    println("║     Security-First Server OS              ║");
    println("╚═══════════════════════════════════════════╝");
    println("");
    println("[INIT] PID 1 — Exploidus Init");
    println("[INIT] Starting system services...");
    println("");

    start_daemon("auditd", "/bin/auditd");
    start_daemon("httpd",  "/bin/httpd");

    println("");
    println("[INIT] All daemons started.");
    println("[INIT] Starting interactive shell...");
    println("");

    println("[INIT] Starting exploish shell...");
    int64_t shell_pid = spawn("/bin/exploish");
    if (shell_pid < 0) {
        println("[INIT] Fatal: could not start shell");
        reboot();
    }
    print("[INIT] Shell PID=");
    print_uint((uint64_t)shell_pid);
    putc('\n');

    println("[INIT] System ready.");
    println("─────────────────────────────────────────────────");
    println("");

    waitpid(shell_pid);

    println("");
    println("[INIT] Session ended. Powering off...");
    sleep_ticks(30);
    poweroff();
}