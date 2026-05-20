#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * cnsl/fim.h — File Integrity Monitoring
 *
 * Watches critical system paths for unauthorized changes.
 * Hooks into VFS write/create — no polling needed.
 *
 * Severity levels:
 *   FIM_CRITICAL — auth files, SSH keys, sudo  (immediate block)
 *   FIM_HIGH     — cron, init binaries
 *   FIM_MEDIUM   — config files
 */

#define FIM_SEV_MEDIUM   0
#define FIM_SEV_HIGH     1
#define FIM_SEV_CRITICAL 2

#define FIM_EVENT_WRITE  0
#define FIM_EVENT_CREATE 1
#define FIM_EVENT_DELETE 2

typedef struct {
    const char *path;
    uint8_t     severity;
    const char *reason;
} fim_alert_t;

/* Initialize FIM — registers watched paths */
void fim_init(void);

/* Called by VFS on every write — returns true if path is critical */
bool fim_on_write(const char *path);

/* Called by VFS on every create */
bool fim_on_create(const char *path);