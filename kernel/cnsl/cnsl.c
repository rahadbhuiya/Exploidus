#include "cnsl.h"
#include "../drivers/serial.h"
#include "../audit/audit.h"
#include <string.h>

/*  Config  */
#define CNSL_MAX_IPS        256
#define CNSL_EVENT_BUF      64
#define CNSL_BLOCK_TABLE    64
#define CNSL_WINDOW_SHORT   180    /* 3 min  */
#define CNSL_WINDOW_MEDIUM  300    /* 5 min  */
#define CNSL_WINDOW_LONG    600    /* 10 min */
#define CNSL_WINDOW_RECON  1800    /* 30 min — persistent_recon */
#define CNSL_COOLDOWN       120    /* 2 min between same alerts */
#define CNSL_NUM_RULES        6    /* rules 0-5 */

/* Tick counter (100 Hz PIT) */
extern uint64_t g_uptime_ticks;
#define NOW()  (g_uptime_ticks / 100)

/*  Structs  */
typedef struct {
    uint64_t ts;
    uint8_t  kind;
} cnsl_event_t;

typedef struct {
    uint32_t     ip;
    cnsl_event_t events[CNSL_EVENT_BUF];
    uint8_t      head;
    uint8_t      count;
    uint64_t     last_alert_ts[CNSL_NUM_RULES];
} ip_buf_t;

/*
 * Block table entry.
 * unblock_at = NOW() + CNSL_BLOCK_DURATION at the time of block.
 * cnsl_tick() expires entries when NOW() >= unblock_at.
 */
typedef struct {
    uint32_t ip;
    bool     active;
    uint64_t unblock_at;
} block_entry_t;

static ip_buf_t      g_ip_table   [CNSL_MAX_IPS];
static block_entry_t g_block_table[CNSL_BLOCK_TABLE];
static uint16_t      g_ip_count    = 0;
static uint16_t      g_block_count = 0;

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
    for (int i = 0; i < g_ip_count; i++)
        if (g_ip_table[i].ip == ip)
            return &g_ip_table[i];

    if (g_ip_count >= CNSL_MAX_IPS) return NULL;

    ip_buf_t *buf = &g_ip_table[g_ip_count++];
    memset(buf, 0, sizeof(ip_buf_t));
    buf->ip = ip;
    return buf;
}

static bool event_in_window(uint64_t ts, uint32_t window_sec)
{
    uint64_t now = NOW();

    if (ts > now) return false;
    return (now - ts) <= window_sec;
}

static uint8_t count_kind(ip_buf_t *buf, uint8_t kind, uint32_t window_sec)
{
    uint8_t  n      = 0;
    for (int i = 0; i < buf->count; i++) {
        cnsl_event_t *ev = &buf->events[i];
        if (event_in_window(ev->ts, window_sec) && ev->kind == kind)
            n++;
    }
    return n;
}

/*
 * count_window_stats — used by persistent_recon.
 *
 * Returns total event count in window and fills:
 *   unique_kinds_mask  — bitmask of seen event kinds
 *   unique_groups_mask — bitmask of seen source groups
 *
 * Source groups (3 groups needed to fire rule):
 *   bit 0 — NETWORK   (SSH_FAIL, SSH_SUCCESS)
 *   bit 1 — WEB       (WEB_SCAN, WEB_EXPLOIT, WEB_AUTH_FAIL)
 *   bit 2 — DB/FW/SYS (DB_AUTH_FAIL, FW_HONEYPOT_PORT, SUDO_FAIL, SU_FAIL)
 */
static uint8_t count_window_stats(ip_buf_t *buf, uint32_t window_sec,
                                  uint16_t *unique_kinds_mask,
                                  uint8_t  *unique_groups_mask)
{
    uint8_t  total  = 0;
    *unique_kinds_mask  = 0;
    *unique_groups_mask = 0;

    for (int i = 0; i < buf->count; i++) {
        cnsl_event_t *ev = &buf->events[i];
        if (!event_in_window(ev->ts, window_sec)) continue;
        total++;
        *unique_kinds_mask |= (uint16_t)(1u << ev->kind);

        switch (ev->kind) {
        case CNSL_KIND_SSH_FAIL:
        case CNSL_KIND_SSH_SUCCESS:
            *unique_groups_mask |= (1u << 0);
            break;
        case CNSL_KIND_WEB_SCAN:
        case CNSL_KIND_WEB_EXPLOIT:
        case CNSL_KIND_WEB_AUTH_FAIL:
            *unique_groups_mask |= (1u << 1);
            break;
        default: /* DB_AUTH_FAIL, FW_HONEYPOT_PORT, SUDO_FAIL, SU_FAIL */
            *unique_groups_mask |= (1u << 2);
            break;
        }
    }
    return total;
}

/* Count set bits in a 16-bit mask */
static uint8_t popcount16(uint16_t v)
{
    uint8_t n = 0;
    while (v) { n += (v & 1u); v >>= 1; }
    return n;
}

static void do_block(uint32_t ip, const char *reason)
{
    /* already blocked? */
    for (int i = 0; i < g_block_count; i++)
        if (g_block_table[i].ip == ip && g_block_table[i].active)
            return;

    /* find a free slot (reuse expired entries) */
    int slot = -1;
    for (int i = 0; i < g_block_count; i++)
        if (!g_block_table[i].active) { slot = i; break; }
    if (slot < 0) {
        if (g_block_count >= CNSL_BLOCK_TABLE) return;
        slot = g_block_count++;
    }

    g_block_table[slot].ip        = ip;
    g_block_table[slot].active    = true;
    g_block_table[slot].unblock_at = NOW() + CNSL_BLOCK_DURATION;

    serial_print("[CNSL] BLOCKED ");
    serial_print(reason);
    serial_print("\n");

    audit_record(AUDIT_CNSL_BLOCK,   0, (uint64_t)ip, 0);
}

static bool check_cooldown(ip_buf_t *buf, uint8_t rule_idx)
{
    uint64_t now  = NOW();
    uint64_t last = buf->last_alert_ts[rule_idx];

    if (last != 0 && now >= last && (now - last) < CNSL_COOLDOWN)
        return false;

    buf->last_alert_ts[rule_idx] = now;
    return true;
}

/*  Rules  */

/* Rule 0: Honeypot port + SSH fail = automated scanner */
static bool rule_honeypot_ssh(ip_buf_t *buf, uint32_t ip)
{
    if (!check_cooldown(buf, 0)) return false;
    if (count_kind(buf, CNSL_KIND_FW_HONEYPOT_PORT, CNSL_WINDOW_SHORT) >= 1 &&
        count_kind(buf, CNSL_KIND_SSH_FAIL,          CNSL_WINDOW_SHORT) >= 1) {
        do_block(ip, "honeypot_then_ssh: automated scanner");
        return true;
    }
    return false;
}

/* Rule 1: SSH fail + DB fail = credential spray */
static bool rule_credential_spray(ip_buf_t *buf, uint32_t ip)
{
    if (!check_cooldown(buf, 1)) return false;
    if (count_kind(buf, CNSL_KIND_SSH_FAIL,     CNSL_WINDOW_MEDIUM) >= 3 &&
        count_kind(buf, CNSL_KIND_DB_AUTH_FAIL, CNSL_WINDOW_MEDIUM) >= 2) {
        do_block(ip, "multi_service_brute_force: credential spray");
        return true;
    }
    return false;
}

/* Rule 2: Web scan + SSH fail = coordinated attack */
static bool rule_web_recon_ssh(ip_buf_t *buf, uint32_t ip)
{
    if (!check_cooldown(buf, 2)) return false;
    uint8_t web = count_kind(buf, CNSL_KIND_WEB_SCAN,    CNSL_WINDOW_LONG)
                + count_kind(buf, CNSL_KIND_WEB_EXPLOIT, CNSL_WINDOW_LONG);
    uint8_t ssh = count_kind(buf, CNSL_KIND_SSH_FAIL,    CNSL_WINDOW_LONG);
    if (web >= 5 && ssh >= 3) {
        do_block(ip, "web_recon_then_ssh: coordinated attack");
        return true;
    }
    return false;
}

/* Rule 3: SSH success + sudo/su fail = privilege escalation */
static bool rule_privesc(ip_buf_t *buf, uint32_t ip)
{
    if (!check_cooldown(buf, 3)) return false;
    uint8_t ssh_ok = count_kind(buf, CNSL_KIND_SSH_SUCCESS, CNSL_WINDOW_MEDIUM);
    uint8_t priv   = count_kind(buf, CNSL_KIND_SUDO_FAIL,   CNSL_WINDOW_MEDIUM)
                   + count_kind(buf, CNSL_KIND_SU_FAIL,     CNSL_WINDOW_MEDIUM);
    if (ssh_ok >= 1 && priv >= 2) {
        do_block(ip, "privilege_escalation: post-login priv escalation");
        return true;
    }
    return false;
}

/* Rule 4: Web auth flood */
static bool rule_web_auth_flood(ip_buf_t *buf, uint32_t ip)
{
    if (!check_cooldown(buf, 4)) return false;
    if (count_kind(buf, CNSL_KIND_WEB_AUTH_FAIL, 120) >= 15) {
        do_block(ip, "web_auth_flood: brute-force web login");
        return true;
    }
    return false;
}

/*
 * Rule 5: Persistent multi-vector reconnaissance (ported from upstream v1.1.0)
 *
 * Fires when a single IP methodically probes the server across multiple
 * attack types and service categories over a 30-minute window:
 *   - total events  >= 20
 *   - unique kinds  >= 4   (not just hammering one service)
 *   - source groups >= 3   (network + web + db/fw/sys)
 *
 * Severity: MEDIUM (not immediate block on its own, but raises alert).
 * The attacker may be slow and spread out to evade threshold-based rules.
 */
static bool rule_persistent_recon(ip_buf_t *buf, uint32_t ip)
{
    if (!check_cooldown(buf, 5)) return false;

    uint16_t kinds_mask  = 0;
    uint8_t  groups_mask = 0;
    uint8_t  total = count_window_stats(buf, CNSL_WINDOW_RECON,
                                        &kinds_mask, &groups_mask);

    uint8_t unique_kinds  = popcount16(kinds_mask);
    uint8_t unique_groups = popcount16((uint16_t)groups_mask);

    if (total >= 20 && unique_kinds >= 4 && unique_groups >= 3) {
        do_block(ip, "persistent_recon: multi-vector 30min recon");
        return true;
    }
    return false;
}

/*  Public: ingest event  */
bool cnsl_ingest(uint32_t src_ip, uint8_t kind)
{
    if (kind >= CNSL_KIND_COUNT) return false;

    /* expire timed-out blocks on every ingest */
    cnsl_tick();

    if (cnsl_is_blocked(src_ip)) return true;

    ip_buf_t *buf = find_or_create_buf(src_ip);
    if (!buf) return false;

    cnsl_event_t *ev = &buf->events[buf->head];
    ev->ts   = NOW();
    ev->kind = kind;
    buf->head = (uint8_t)((buf->head + 1) % CNSL_EVENT_BUF);
    if (buf->count < CNSL_EVENT_BUF) buf->count++;

    /* highest confidence rules first */
    if (rule_honeypot_ssh    (buf, src_ip)) return true;
    if (rule_credential_spray(buf, src_ip)) return true;
    if (rule_web_recon_ssh   (buf, src_ip)) return true;
    if (rule_privesc         (buf, src_ip)) return true;
    if (rule_web_auth_flood  (buf, src_ip)) return true;
    if (rule_persistent_recon(buf, src_ip)) return true;

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

/*  Public: manual unblock  */
bool cnsl_unblock(uint32_t ip)
{
    for (int i = 0; i < g_block_count; i++) {
        if (g_block_table[i].ip == ip && g_block_table[i].active) {
            g_block_table[i].active = false;
            serial_print("[CNSL] UNBLOCKED (manual)\n");
            audit_record(AUDIT_CNSL_UNBLOCK, 0, (uint64_t)ip, 0);
            return true;
        }
    }
    return false;   /* IP was not blocked */
}

/*  Public: TTL until auto-unblock  */
uint64_t cnsl_get_block_ttl(uint32_t ip)
{
    uint64_t now = NOW();
    for (int i = 0; i < g_block_count; i++) {
        if (g_block_table[i].ip == ip && g_block_table[i].active) {
            return (g_block_table[i].unblock_at > now)
                   ? g_block_table[i].unblock_at - now
                   : 0;
        }
    }
    return 0;
}

/*  Public: expire timed-out blocks  */
void cnsl_tick(void)
{
    uint64_t now = NOW();
    for (int i = 0; i < g_block_count; i++) {
        if (g_block_table[i].active && now >= g_block_table[i].unblock_at) {
            g_block_table[i].active = false;
            serial_print("[CNSL] UNBLOCKED (auto-expire)\n");
            audit_record(AUDIT_CNSL_UNBLOCK, 0, (uint64_t)g_block_table[i].ip, 0);
        }
    }
}

/*  Public: list active blocks for userspace  */
uint16_t cnsl_list(cnsl_list_entry_t *out, uint16_t max)
{
    uint16_t n = 0;
    for (int i = 0; i < g_block_count && n < max; i++) {
        if (!g_block_table[i].active) continue;
        out[n].ip       = g_block_table[i].ip;
        out[n].ttl_secs = cnsl_get_block_ttl(g_block_table[i].ip);
        n++;
    }
    return n;
}
