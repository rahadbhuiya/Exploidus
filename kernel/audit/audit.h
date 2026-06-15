#pragma once
#include <stdint.h>

typedef enum {
    AUDIT_CAP_CREATE   = 0,
    AUDIT_CAP_DENIED   = 1,
    AUDIT_CAP_FORGERY  = 2,
    AUDIT_CAP_REVOKE   = 3,
    AUDIT_CAP_DELEGATE = 4,
    AUDIT_SYSCALL      = 5,
    AUDIT_PROC_FORK    = 6,
    AUDIT_PROC_EXEC    = 7,
    AUDIT_PROC_EXIT    = 8,
    AUDIT_MEM_ALLOC    = 9,
    AUDIT_MEM_FREE     = 10,
    AUDIT_FILE_OPEN    = 11,
    AUDIT_FILE_WRITE   = 12,
    AUDIT_NET_SEND     = 13,
    AUDIT_NET_RECV     = 14,
    AUDIT_CNSL_BLOCK   = 15,
    AUDIT_CNSL_UNBLOCK = 16,
} audit_event_t;

typedef struct {
    uint64_t      timestamp;   /* kernel tick */
    uint32_t      pid;
    audit_event_t event;
    uint64_t      arg0;
    uint64_t      arg1;
} audit_entry_t;

#define AUDIT_RING_SIZE 4096   /* must be power of two */

void              audit_init(void);
void              audit_record(audit_event_t ev, uint32_t pid,
                               uint64_t arg0, uint64_t arg1);
const audit_entry_t *audit_read(uint32_t *count_out);
uint64_t          audit_total(void);