#include "cnsl.h"
#include "../drivers/serial.h"
#include "../audit/audit.h"
#include <string.h>

/* Config  */
#define CNSL_MAX_IPS        256   /* max tracked IPs */
#define CNSL_EVENT_BUF      64    /* events per IP (circular) */
#define CNSL_BLOCK_TABLE    64    /* max blocked IPs */
#define CNSL_WINDOW_SHORT   180   /* 3 min  */
#define CNSL_WINDOW_MEDIUM  300   /* 5 min  */
#define CNSL_WINDOW_LONG    600   /* 10 min */
#define CNSL_COOLDOWN       120   /* 2 min between same alerts */

/*  Tick counter (incremented by sched_tick)  */
extern uint64_t g_uptime_ticks;
#define NOW()  (g_uptime_ticks / 100)   /* ticks → seconds (100Hz PIT) */

/*  Event entry  */
typedef struct {
    uint64_t ts;     /* seconds since boot */
    uint8_t  kind;
} cnsl_event_t;

/*  Per-IP buffer  */
typedef struct {
    uint32_t      ip;
    cnsl_event_t  events[CNSL_EVENT_BUF];
    uint8_t       head;      /* next write index (circular) */
    uint8_t       count;     /* how many valid entries */
    uint64_t      last_alert_ts[9];  /* cooldown per rule index */
} ip_buf_t;



/*  Block table  */
typedef struct {
    uint32_t ip;
    bool     active;
} block_entry_t;

static ip_buf_t     g_ip_table[CNSL_MAX_IPS];
static block_entry_t g_block_table[CNSL_BLOCK_TABLE];
static uint16_t g_ip_count    = 0;
static uint16_t g_block_count = 0;



/*  Init  */
void cnsl_init(void)
{
    memset(g_ip_table,    0, sizeof(g_ip_table));
    memset(g_block_table, 0, sizeof(g_block_table));
    g_ip_count    = 0;
    g_block_count = 0;
    serial_print("[CNSL] Correlated Network Security Layer initialized\n");
}


/*  Helpers  */
static ip_buf_t *find_or_create_buf(uint32_t ip)
{
    /* find existing */
    for (int i = 0; i < g_ip_count; i++)
        if (g_ip_table[i].ip == ip)
            return &g_ip_table[i];

    /* create new */
    if (g_ip_count >= CNSL_MAX_IPS)
        return NULL;   /* table full */

    ip_buf_t *buf = &g_ip_table[g_ip_count++];
    memset(buf, 0, sizeof(ip_buf_t));
    buf->ip = ip;
    return buf;
}

static uint8_t count_kind(ip_buf_t *buf, uint8_t kind, uint32_t window_sec)
{
    uint64_t cutoff = NOW() - window_sec;
    uint8_t  n      = 0;

    for (int i = 0; i < buf->count; i++) {
        cnsl_event_t *ev = &buf->events[i];
        if (ev->ts > cutoff && ev->kind == kind)
            n++;
    }
    return n;
}

static void block_ip(uint32_t ip, const char *reason)
{
    /* already blocked? */
    for (int i = 0; i < g_block_count; i++)
        if (g_block_table[i].ip == ip && g_block_table[i].active)
            return;

    if (g_block_count < CNSL_BLOCK_TABLE) {
        g_block_table[g_block_count].ip     = ip;
        g_block_table[g_block_count].active = true;
        g_block_count++;
    }

    serial_print("[CNSL] BLOCKED IP: ");
    serial_print(reason);
    serial_print("\n");
}

static bool check_cooldown(ip_buf_t *buf, uint8_t rule_idx)
{
    if (NOW() - buf->last_alert_ts[rule_idx] < CNSL_COOLDOWN)
        return false;
    buf->last_alert_ts[rule_idx] = NOW();
    return true;
}

/*  Rules  */

/* Rule 0: Honeypot port + SSH fail = automated scanner */
static bool rule_honeypot_ssh(ip_buf_t *buf, uint32_t ip)
{
    if (!check_cooldown(buf, 0)) return false;

    uint8_t honeypot = count_kind(buf, CNSL_KIND_FW_HONEYPOT_PORT, CNSL_WINDOW_SHORT);
    uint8_t ssh_fail = count_kind(buf, CNSL_KIND_SSH_FAIL,         CNSL_WINDOW_SHORT);

    if (honeypot >= 1 && ssh_fail >= 1) {
        block_ip(ip, "honeypot_then_ssh: automated scanner");
        return true;
    }
    return false;
}

/* Rule 1: SSH fail + DB fail = credential spray */
static bool rule_credential_spray(ip_buf_t *buf, uint32_t ip)
{
    if (!check_cooldown(buf, 1)) return false;

    uint8_t ssh_fail = count_kind(buf, CNSL_KIND_SSH_FAIL,     CNSL_WINDOW_MEDIUM);
    uint8_t db_fail  = count_kind(buf, CNSL_KIND_DB_AUTH_FAIL, CNSL_WINDOW_MEDIUM);

    if (ssh_fail >= 3 && db_fail >= 2) {
        block_ip(ip, "multi_service_brute_force: credential spray");
        return true;
    }
    return false;
}

/* Rule 2: Web scan + SSH fail = coordinated attack */
static bool rule_web_recon_ssh(ip_buf_t *buf, uint32_t ip)
{
    if (!check_cooldown(buf, 2)) return false;

    uint8_t web_scan = count_kind(buf, CNSL_KIND_WEB_SCAN,    CNSL_WINDOW_LONG);
    uint8_t web_exp  = count_kind(buf, CNSL_KIND_WEB_EXPLOIT, CNSL_WINDOW_LONG);
    uint8_t ssh_fail = count_kind(buf, CNSL_KIND_SSH_FAIL,    CNSL_WINDOW_LONG);

    if ((web_scan + web_exp) >= 5 && ssh_fail >= 3) {
        block_ip(ip, "web_recon_then_ssh: coordinated attack");
        return true;
    }
    return false;
}

/* Rule 3: SSH success + sudo/su fail = privilege escalation */
static bool rule_privesc(ip_buf_t *buf, uint32_t ip)
{
    if (!check_cooldown(buf, 3)) return false;

    uint8_t ssh_ok    = count_kind(buf, CNSL_KIND_SSH_SUCCESS, CNSL_WINDOW_MEDIUM);
    uint8_t sudo_fail = count_kind(buf, CNSL_KIND_SUDO_FAIL,   CNSL_WINDOW_MEDIUM);
    uint8_t su_fail   = count_kind(buf, CNSL_KIND_SU_FAIL,     CNSL_WINDOW_MEDIUM);

    if (ssh_ok >= 1 && (sudo_fail + su_fail) >= 2) {
        block_ip(ip, "privilege_escalation: post-login priv escalation");
        return true;
    }
    return false;
}

/* Rule 4: Web auth flood */
static bool rule_web_auth_flood(ip_buf_t *buf, uint32_t ip)
{
    if (!check_cooldown(buf, 4)) return false;

    uint8_t auth_fail = count_kind(buf, CNSL_KIND_WEB_AUTH_FAIL, 120);

    if (auth_fail >= 15) {
        block_ip(ip, "web_auth_flood: brute-force web login");
        return true;
    }
    return false;
}

/*  Public: ingest event  */
bool cnsl_ingest(uint32_t src_ip, uint8_t kind)
{
    if (kind >= CNSL_KIND_COUNT) return false;

    /* already blocked? fast path */
    if (cnsl_is_blocked(src_ip)) return true;

    ip_buf_t *buf = find_or_create_buf(src_ip);
    if (!buf) return false;

    /* write event into circular buffer */
    cnsl_event_t *ev = &buf->events[buf->head];
    ev->ts   = NOW();
    ev->kind = kind;
    buf->head = (buf->head + 1) % CNSL_EVENT_BUF;
    if (buf->count < CNSL_EVENT_BUF) buf->count++;

    /* evaluate rules — highest confidence first */
    if (rule_honeypot_ssh    (buf, src_ip)) return true;
    if (rule_credential_spray(buf, src_ip)) return true;
    if (rule_web_recon_ssh   (buf, src_ip)) return true;
    if (rule_privesc         (buf, src_ip)) return true;
    if (rule_web_auth_flood  (buf, src_ip)) return true;

    return false;
}


/*  Public: is IP blocked?  */
bool cnsl_is_blocked(uint32_t ip)
{
    for (int i = 0; i < g_block_count; i++)
        if (g_block_table[i].ip == ip && g_block_table[i].active)
            return true;
    return false;
}