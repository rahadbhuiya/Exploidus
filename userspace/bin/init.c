/*
 * init.c — Exploidus Init System
 *
 * PID 1. First userspace process.
 * Spawns all system daemons in the correct order,
 * then monitors them and restarts if they crash.
 *
 * Boot order:
 *   1. auditd   — MUST start first (security logging)
 *   2. [future: sshd, httpd, cnsl-daemon]
 *   3. exploish — interactive shell (last)
 *
 * If a daemon crashes, init restarts it after a delay.
 * If exploish exits, init reboots the system.
 */

#include "../libc/syscall.h"

/* ─── Helpers ─────────────────────────────────────────────────────── */
static void print(const char *s) { puts(s); }
static void println(const char *s) { puts(s); putc('\n'); }
static void print_uint(uint64_t n)
{
    if (n == 0) { putc('0'); return; }
    char buf[21]; int i = 0;
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) putc(buf[i]);
}

/* ─── Daemon table ────────────────────────────────────────────────── */
#define MAX_DAEMONS 8
#define RESTART_DELAY 100   /* ticks before restart */

typedef struct {
    const char *path;        /* ELF path on disk */
    const char *name;        /* human-readable name */
    int64_t     pid;         /* current PID, -1 = not running */
    int         restart;     /* 1 = auto-restart on crash */
    int         started;     /* 1 = has been started at least once */
} daemon_t;

static daemon_t g_daemons[] = {
    { "/bin/auditd",  "auditd",   -1, 1, 0 },
    /* Future daemons go here:
    { "/bin/sshd",    "sshd",     -1, 1, 0 },
    { "/bin/httpd",   "httpd",    -1, 1, 0 },
    { "/bin/cnsl",    "cnsl",     -1, 1, 0 },
    */
};

#define DAEMON_COUNT (sizeof(g_daemons) / sizeof(g_daemons[0]))

/* Shell is special — if it exits, we reboot */
static int64_t g_shell_pid = -1;

/* ─── Banner ──────────────────────────────────────────────────────── */
static void print_banner(void)
{
    println("");
    println("╔═══════════════════════════════════════════╗");
    println("║     Exploidus Init System  v0.1.0         ║");
    println("║     Security-First Server OS              ║");
    println("╚═══════════════════════════════════════════╝");
    println("");
}

/* ─── Start one daemon ────────────────────────────────────────────── */
static int64_t start_daemon(daemon_t *d)
{
    print("[INIT] Starting ");
    print(d->name);
    print(" ... ");

    int64_t pid = spawn(d->path);
    if (pid < 0) {
        println("FAILED");
        return -1;
    }

    print("PID=");
    print_uint((uint64_t)pid);
    putc('\n');

    d->pid     = pid;
    d->started = 1;
    return pid;
}

/* ─── Start shell ─────────────────────────────────────────────────── */
static void start_shell(void)
{
    println("[INIT] Starting exploish shell...");
    int64_t pid = spawn("/bin/exploish");
    if (pid < 0) {
        println("[INIT] Fatal: could not start shell");
        reboot();
    }
    g_shell_pid = pid;
    print("[INIT] Shell PID=");
    print_uint((uint64_t)pid);
    putc('\n');
}

/* ─── Main ────────────────────────────────────────────────────────── */
void main(void)
{
    print_banner();

    println("[INIT] PID 1 — Exploidus Init");
    println("[INIT] Starting system services...");
    println("");

    /* Phase 1: Start all daemons in order */
    for (uint64_t i = 0; i < DAEMON_COUNT; i++) {
        start_daemon(&g_daemons[i]);
    }

    println("");
    println("[INIT] All daemons started.");
    println("[INIT] Starting interactive shell...");
    println("");

    /* Phase 2: Start shell */
    start_shell();

    println("[INIT] System ready.");
    println("─────────────────────────────────────────────────");
    println("");

    /* Phase 3: Wait for shell to exit, then poweroff */
    waitpid(g_shell_pid);

    println("");
    println("[INIT] Session ended. Powering off...");
    sleep_ticks(30);
    poweroff();
}