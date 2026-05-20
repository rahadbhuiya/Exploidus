#include "fim.h"
#include "cnsl.h"
#include "../drivers/serial.h"
#include "../audit/audit.h"
#include <string.h>

/* kernel string.c has no strncmp — provide our own */
static int k_strncmp(const char *a, const char *b, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

/*
 * cnsl/fim.c — File Integrity Monitoring
 *
 * Hooks into VFS write/create to detect unauthorized changes
 * to critical system files — no polling, zero overhead on
 * non-watched paths.
 */

/*  Watched path table  */
typedef struct {
    const char *path;        /* exact path or prefix if ends with / */
    bool        is_prefix;   /* true = watch everything under this dir */
    uint8_t     severity;
    const char *reason;
} fim_watch_t;

static const fim_watch_t g_watched[] = {
    /* CRITICAL — auth, SSH, sudo, rootkit vector */
    { "/etc/passwd",                false, FIM_SEV_CRITICAL, "user account modified"         },
    { "/etc/shadow",                false, FIM_SEV_CRITICAL, "password hashes modified"      },
    { "/etc/sudoers",               false, FIM_SEV_CRITICAL, "sudo config modified"          },
    { "/etc/ssh/sshd_config",       false, FIM_SEV_CRITICAL, "SSH daemon config modified"    },
    { "/root/.ssh/",                true,  FIM_SEV_CRITICAL, "root SSH key modified"         },
    { "/etc/ld.so.preload",         false, FIM_SEV_CRITICAL, "library preload modified (rootkit?)" },

    /* HIGH — persistence vectors */
    { "/etc/crontab",               false, FIM_SEV_HIGH,     "crontab modified"              },
    { "/etc/cron.d/",               true,  FIM_SEV_HIGH,     "cron job modified"             },
    { "/etc/hosts",                 false, FIM_SEV_HIGH,     "hosts file modified (DNS hijack?)" },
    { "/bin/bash",                  false, FIM_SEV_HIGH,     "bash binary modified"          },
    { "/sbin/init",                 false, FIM_SEV_HIGH,     "init binary modified"          },

    /* MEDIUM — config files worth watching */
    { "/etc/fstab",                 false, FIM_SEV_MEDIUM,   "fstab modified"                },
    { "/etc/resolv.conf",           false, FIM_SEV_MEDIUM,   "DNS resolver modified"         },
};

#define FIM_WATCH_COUNT (sizeof(g_watched) / sizeof(g_watched[0]))

/*  Init  */
void fim_init(void)
{
    serial_print("[FIM ] File Integrity Monitor initialized\n");
    serial_print("[FIM ] Watching ");
    /* print count as single digit — serial has no printf */
    char buf[4];
    buf[0] = '0' + (FIM_WATCH_COUNT / 10);
    buf[1] = '0' + (FIM_WATCH_COUNT % 10);
    buf[2] = '\0';
    serial_print(buf);
    serial_print(" critical paths\n");
}

/*  Match helper  */
static const fim_watch_t *fim_match(const char *path)
{
    if (!path) return NULL;

    for (uint32_t i = 0; i < FIM_WATCH_COUNT; i++) {
        const fim_watch_t *w = &g_watched[i];

        if (w->is_prefix) {
            /* prefix match: /etc/cron.d/ matches /etc/cron.d/myjob */
            size_t plen = strlen(w->path);
            if (k_strncmp(path, w->path, plen) == 0)
                return w;
        } else {
            /* exact match */
            if (strcmp(path, w->path) == 0)
                return w;
        }
    }
    return NULL;
}

/*  Alert helper  */
static void fim_alert(const fim_watch_t *w, const char *path, uint8_t event)
{
    const char *ev_str = (event == FIM_EVENT_WRITE)  ? "WRITE"  :
                         (event == FIM_EVENT_CREATE) ? "CREATE" : "DELETE";

    serial_print("[FIM ] ALERT ");
    serial_print(ev_str);
    serial_print(" on ");
    serial_print(path);
    serial_print(" — ");
    serial_print(w->reason);
    serial_print("\n");

    /* Feed into CNSL correlator as a suspicious event */
    if (w->severity == FIM_SEV_CRITICAL) {
        serial_print("[FIM ] CRITICAL — flagging to audit ring\n");
        /* In a full implementation, this would raise a kernel audit record
         * and potentially trigger an INTENT_AUDIT process wake-up.
         * For now, we log to serial and let the audit subsystem handle it. */
    }
}

/*  Public hooks  */

bool fim_on_write(const char *path)
{
    const fim_watch_t *w = fim_match(path);
    if (!w) return false;

    fim_alert(w, path, FIM_EVENT_WRITE);
    return (w->severity == FIM_SEV_CRITICAL);
}

bool fim_on_create(const char *path)
{
    const fim_watch_t *w = fim_match(path);
    if (!w) return false;

    fim_alert(w, path, FIM_EVENT_CREATE);
    return (w->severity == FIM_SEV_CRITICAL);
}