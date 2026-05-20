/*
 * auditd.c — Exploidus Audit Daemon
 *
 * Reads the kernel audit ring via SYS_AUDIT_DUMP syscall
 * and writes human-readable log entries to /var/log/audit.log
 *
 * Runs as INTENT_AUDIT process — highest scheduler priority.
 * Polls every 500ms, logs new events since last read.
 *
 * Event types:
 *   0  CAP_CREATE      — capability token created
 *   1  CAP_DENIED      — capability check failed (suspicious)
 *   2  CAP_FORGERY     — forged token detected (CRITICAL)
 *   3  CAP_REVOKE      — capability revoked
 *   4  CAP_DELEGATE    — capability delegated
 *   5  SYSCALL         — syscall issued
 *   6  PROC_FORK       — process forked
 *   7  PROC_EXEC       — process exec'd
 *   8  PROC_EXIT       — process exited
 *   9  MEM_ALLOC       — memory allocated
 *   10 MEM_FREE        — memory freed
 *   11 FILE_OPEN       — file opened
 *   12 FILE_WRITE      — file written (FIM triggers here)
 *   13 NET_SEND        — network packet sent
 *   14 NET_RECV        — network packet received
 */

#include "../libc/syscall.h"

/* ─── Config ──────────────────────────────────────────────────────── */
#define AUDIT_BUF_SIZE   256
#define POLL_INTERVAL    50    /* ticks (~500ms at 100Hz) */
#define LOG_PATH         "/var/log/audit.log"

/* ─── Helpers ─────────────────────────────────────────────────────── */
static void println(const char *s) { puts(s); putc('\n'); }

static void print_uint(uint64_t n)
{
    if (n == 0) { putc('0'); return; }
    char buf[21]; int i = 0;
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) putc(buf[i]);
}

static void print_hex(uint64_t n)
{
    const char *hex = "0123456789abcdef";
    puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        putc(hex[(n >> i) & 0xF]);
}

static const char *event_name(uint32_t ev)
{
    switch (ev) {
        case 0:  return "CAP_CREATE  ";
        case 1:  return "CAP_DENIED  ";
        case 2:  return "CAP_FORGERY ";  /* CRITICAL */
        case 3:  return "CAP_REVOKE  ";
        case 4:  return "CAP_DELEGATE";
        case 5:  return "SYSCALL     ";
        case 6:  return "PROC_FORK   ";
        case 7:  return "PROC_EXEC   ";
        case 8:  return "PROC_EXIT   ";
        case 9:  return "MEM_ALLOC   ";
        case 10: return "MEM_FREE    ";
        case 11: return "FILE_OPEN   ";
        case 12: return "FILE_WRITE  ";
        case 13: return "NET_SEND    ";
        case 14: return "NET_RECV    ";
        default: return "UNKNOWN     ";
    }
}

static int is_critical(uint32_t ev)
{
    /* CAP_FORGERY and CAP_DENIED are security-critical */
    return (ev == 1 || ev == 2);
}

/* ─── Log one entry to stdout + log file ─────────────────────────── */
static void log_entry(int logfd, const audit_entry_user_t *e)
{
    /* Format: [TICK] PID=N EVENT arg0=X arg1=X */
    const char *prefix = is_critical(e->event) ? "[AUDITD][WARN] " : "[AUDITD] ";

    /* Write to stdout */
    puts(prefix);
    puts("tick="); print_uint(e->timestamp);
    puts(" pid=");  print_uint(e->pid);
    puts(" evt=");  puts(event_name(e->event));
    puts(" a0=");   print_hex(e->arg0);
    puts(" a1=");   print_hex(e->arg1);
    putc('\n');

    /* Also write to log file if open */
    if (logfd >= 0) {
        write(logfd, prefix, strlen(prefix));
        /* simplified — just write event name to file */
        write(logfd, event_name(e->event), 12);
        write(logfd, "\n", 1);
    }
}

/* ─── Main ────────────────────────────────────────────────────────── */
void main(void)
{
    println("=== Exploidus Audit Daemon (auditd) ===");
    println("Polling kernel audit ring...");
    println("");

    /* Open log file */
    int logfd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (logfd < 0) {
        println("[AUDITD] Warning: could not open /var/log/audit.log");
        println("[AUDITD] Logging to stdout only");
    } else {
        println("[AUDITD] Logging to /var/log/audit.log");
    }

    /* Static buffer — no mmap needed */
    static audit_entry_user_t buf[AUDIT_BUF_SIZE];

    uint64_t last_total = 0;
    uint64_t seen       = 0;

    println("[AUDITD] Started. Watching for security events...");
    println("─────────────────────────────────────────────────");

    while (1) {
        /*
         * SYS_AUDIT_DUMP returns the ring contents.
         * We use a null capability for now — in production
         * auditd would hold a CAP_RIGHT_AUDIT capability token.
         */
        cap_token_t null_cap = {0, 0};
        int64_t count = audit_dump(null_cap, buf, AUDIT_BUF_SIZE);

        if (count > 0 && (uint64_t)count > last_total) {
            /* Print only new entries since last poll */
            uint64_t start = last_total < (uint64_t)count
                           ? last_total : 0;

            for (uint64_t i = start; i < (uint64_t)count; i++) {
                log_entry(logfd, &buf[i]);
                seen++;

                /* Extra warning for critical events */
                if (is_critical(buf[i].event)) {
                    println("[AUDITD] *** SECURITY ALERT — review above event ***");
                }
            }
            last_total = (uint64_t)count;
        }

        /* Sleep between polls */
        sleep_ticks(POLL_INTERVAL);
    }

    /* Never reached */
    if (logfd >= 0) close(logfd);
    exit(0);
}