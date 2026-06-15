#pragma once
#include <stdint.h>
#include <stdbool.h>

/*  Event kinds  */
#define CNSL_KIND_SSH_FAIL          0
#define CNSL_KIND_SSH_SUCCESS       1
#define CNSL_KIND_DB_AUTH_FAIL      2
#define CNSL_KIND_WEB_SCAN          3
#define CNSL_KIND_WEB_EXPLOIT       4
#define CNSL_KIND_WEB_AUTH_FAIL     5
#define CNSL_KIND_FW_HONEYPOT_PORT  6
#define CNSL_KIND_SUDO_FAIL         7
#define CNSL_KIND_SU_FAIL           8
#define CNSL_KIND_COUNT             9

/*  Severity  */
#define CNSL_SEV_LOW    0
#define CNSL_SEV_MEDIUM 1
#define CNSL_SEV_HIGH   2

/*  Block duration: auto-expire after this many seconds (default 30 min)  */
#define CNSL_BLOCK_DURATION  1800

/*  Alert  */
typedef struct {
    const char *rule_name;
    uint32_t    src_ip;       /* network byte order */
    uint8_t     severity;
    uint8_t     confidence;   /* 0-100 */
    char        description[128];
} cnsl_alert_t;

/*  Public API  */
void     cnsl_init        (void);
bool     cnsl_ingest      (uint32_t src_ip, uint8_t kind);
bool     cnsl_is_blocked  (uint32_t src_ip);

/*  Unblock — manual (shell) or automatic (timer via cnsl_tick)  */
bool     cnsl_unblock     (uint32_t src_ip);

/*  Returns seconds until auto-unblock, or 0 if not blocked  */
uint64_t cnsl_get_block_ttl(uint32_t src_ip);

/*  Called by scheduler or ingest to expire timed-out blocks  */
void     cnsl_tick        (void);

/*  List active blocked IPs — for userspace cnsl-list command.
 *  Fills up to max entries. Returns count written.            */
typedef struct {
    uint32_t ip;        /* network byte order */
    uint64_t ttl_secs;  /* seconds until auto-unblock */
} cnsl_list_entry_t;

uint16_t cnsl_list(cnsl_list_entry_t *out, uint16_t max);