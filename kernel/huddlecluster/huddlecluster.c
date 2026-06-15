#include "huddlecluster.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"
#include <string.h>

/*
 * huddlecluster.c — Kernel port of HuddleCluster v1.3.0
 *
 * Penguin-inspired self-organizing load balancer.
 * All values use integer math (0-100 scale) instead of floats.
 */

extern uint64_t g_uptime_ticks;

/*  Helpers  */

static void hc_log(const char *msg, const char *id, uint8_t temp)
{
    serial_print("[HC] ");
    serial_print(msg);
    if (id && id[0]) {
        serial_print(" ");
        serial_print(id);
    }
    if (temp) {
        serial_print(" temp=");
        serial_printhex((uint64_t)temp);
    }
    serial_print("\n");
}

static uint32_t hc_window_avg(hc_metrics_t *m)
{
    if (!m->window_count) return 0;
    uint64_t sum = 0;
    for (uint8_t i = 0; i < m->window_count; i++)
        sum += m->latency_window[i];
    return (uint32_t)(sum / m->window_count);
}

/* EMA: new = alpha*raw + (1-alpha)*old  (alpha = HC_EMA_ALPHA_PCT / 100) */
static uint8_t ema(uint8_t old_val, uint8_t raw)
{
    uint32_t result = (HC_EMA_ALPHA_PCT * raw + (100 - HC_EMA_ALPHA_PCT) * old_val) / 100;
    return (uint8_t)(result > 100 ? 100 : result);
}

/*  Temperature calculation  */

static void hc_update_temperature(hc_server_t *s)
{
    hc_metrics_t *m = &s->metrics;

    /* Compute raw temperature (0-100) */
    uint32_t raw = 0;
    raw += (m->cpu_pct * 10) / 100;            /* W=0.10 */
    raw += (m->mem_pct * 5)  / 100;            /* W=0.05 */

    uint32_t conn_score = m->active_conns > 1000 ? 100 : m->active_conns / 10;
    raw += (conn_score * 10) / 100;            /* W=0.10 */

    raw += (m->anomaly_score * 70) / 100;      /* W=0.70 — latency anomaly */
    raw += (m->error_rate_pct * 5) / 100;      /* W=0.05 */

    if (raw > 100) raw = 100;

    /* Apply EMA smoothing — prevents oscillation */
    s->temperature = ema(s->temperature, (uint8_t)raw);
}

/* Update latency anomaly for all inner servers vs cluster median */
static void hc_update_anomaly_scores(hc_cluster_t *c)
{
    if (!c->inner_count) return;

    /* Compute cluster median latency from inner ring */
    uint32_t avgs[HC_MAX_INNER];
    uint8_t  n = 0;
    for (uint8_t i = 0; i < c->inner_count; i++) {
        hc_server_t *s = &c->servers[c->inner[i]];
        uint32_t avg = hc_window_avg(&s->metrics);
        if (avg > 0) avgs[n++] = avg;
    }
    if (!n) return;

    /* Simple sort for median */
    for (uint8_t i = 0; i < n - 1; i++)
        for (uint8_t j = i + 1; j < n; j++)
            if (avgs[j] < avgs[i]) { uint32_t t = avgs[i]; avgs[i] = avgs[j]; avgs[j] = t; }

    uint32_t median = avgs[n / 2];
    if (!median) return;

    /* Update each inner server's anomaly score */
    for (uint8_t i = 0; i < c->inner_count; i++) {
        hc_server_t *s = &c->servers[c->inner[i]];
        uint32_t avg = hc_window_avg(&s->metrics);
        if (!avg) { s->metrics.anomaly_score = 0; continue; }

        /* anomaly = clamp((ratio - 1.0) / 2.0, 0, 1) * 100 */
        /* ratio = avg / median */
        if (avg <= median) {
            s->metrics.anomaly_score = 0;
        } else {
            uint32_t ratio_x100 = (avg * 100) / median;  /* e.g. 200 = 2x */
            uint32_t score = (ratio_x100 - 100) / 2;      /* (ratio-1)/2 * 100 */
            s->metrics.anomaly_score = (uint32_t)(score > 100 ? 100 : score);
        }
    }
}

/*  Outer ring helpers  */

/* Sort outer ring by temperature (coolest first) */
static void hc_sort_outer(hc_cluster_t *c)
{
    for (uint8_t i = 0; i < c->outer_count - 1; i++) {
        for (uint8_t j = i + 1; j < c->outer_count; j++) {
            if (c->servers[c->outer[j]].temperature < c->servers[c->outer[i]].temperature) {
                uint8_t t = c->outer[i];
                c->outer[i] = c->outer[j];
                c->outer[j] = t;
            }
        }
    }
}

static void hc_log_rotation(hc_cluster_t *c, const char *id,
                             uint8_t dir, uint8_t reason, uint8_t temp)
{
    hc_rotation_event_t *ev = &c->log[c->log_head % HC_ROTATION_LOG];
    memcpy(ev->server_id, id, 16);
    ev->direction   = dir;
    ev->reason      = reason;
    ev->temperature = temp;
    c->log_head++;
    c->total_rotations++;
}

/*  Move helpers  */

static void hc_move_to_outer(hc_cluster_t *c, uint8_t inner_idx)
{
    uint8_t srv_idx = c->inner[inner_idx];
    hc_server_t *s  = &c->servers[srv_idx];

    s->position          = HC_OUTER;
    s->total_inner_ticks += g_uptime_ticks - s->last_rotated_tick;
    s->last_rotated_tick  = g_uptime_ticks;
    s->rotation_count++;

    /* Remove from inner ring */
    for (uint8_t i = inner_idx; i < c->inner_count - 1; i++)
        c->inner[i] = c->inner[i + 1];
    c->inner_count--;

    /* Adjust head pointer */
    if (c->inner_head > inner_idx && c->inner_head > 0)
        c->inner_head--;
    if (c->inner_count > 0)
        c->inner_head %= c->inner_count;

    /* Add to outer */
    c->outer[c->outer_count++] = srv_idx;
    hc_sort_outer(c);

    hc_log_rotation(c, s->id, 0, 0, s->temperature);
    hc_log("EVICT inner→outer", s->id, s->temperature);
}

static void hc_move_to_inner(hc_cluster_t *c, uint8_t outer_idx)
{
    uint8_t srv_idx = c->outer[outer_idx];
    hc_server_t *s  = &c->servers[srv_idx];

    s->position          = HC_INNER;
    s->total_outer_ticks += g_uptime_ticks - s->last_rotated_tick;
    s->last_rotated_tick  = g_uptime_ticks;
    s->rotation_count++;

    /* Remove from outer */
    for (uint8_t i = outer_idx; i < c->outer_count - 1; i++)
        c->outer[i] = c->outer[i + 1];
    c->outer_count--;

    /* Add to inner */
    c->inner[c->inner_count++] = srv_idx;

    hc_log_rotation(c, s->id, 1, 1, s->temperature);
    hc_log("PROMOTE outer→inner", s->id, s->temperature);
}

/*  Public API  */

void hc_init(hc_cluster_t *c)
{
    memset(c, 0, sizeof(hc_cluster_t));
    hc_log("HuddleCluster initialized", NULL, 0);
}

bool hc_add_server(hc_cluster_t *c, const char *id,
                   uint32_t ip, uint16_t port, uint8_t weight)
{
    if (c->server_count >= HC_MAX_SERVERS) return false;

    uint8_t idx = c->server_count++;
    hc_server_t *s = &c->servers[idx];

    memset(s, 0, sizeof(hc_server_t));
    memcpy(s->id, id, 15);
    s->ip              = ip;
    s->port            = port;
    s->weight          = weight ? weight : 1;
    s->position        = HC_OUTER;
    s->active          = true;
    s->metrics.healthy = true;
    s->last_rotated_tick = g_uptime_ticks;

    /* Start in outer ring */
    c->outer[c->outer_count++] = idx;

    /* Auto-promote to inner if space available */
    if (c->inner_count < HC_MIN_INNER) {
        hc_move_to_inner(c, c->outer_count - 1);
    }

    hc_log("Added server", s->id, 0);
    return true;
}

hc_server_t *hc_get_server(hc_cluster_t *c)
{
    if (!c->inner_count) {
        /* Emergency: pick any available server */
        if (c->server_count)
            return &c->servers[0];
        return NULL;
    }

    /* Round-robin from inner ring */
    uint8_t idx = c->inner[c->inner_head];
    c->inner_head = (c->inner_head + 1) % c->inner_count;
    return &c->servers[idx];
}

void hc_record_latency(hc_cluster_t *c, hc_server_t *s, uint32_t latency_ms)
{
    hc_metrics_t *m = &s->metrics;

    /* Push to rolling window */
    m->latency_window[m->window_head % HC_LATENCY_WINDOW] = latency_ms;
    m->window_head++;
    if (m->window_count < HC_LATENCY_WINDOW) m->window_count++;

    /* Update avg */
    m->avg_latency_ms = hc_window_avg(m);

    /* Update cluster anomaly scores */
    hc_update_anomaly_scores(c);

    /* Recalculate temperature */
    hc_update_temperature(s);
}

void hc_update_metrics(hc_server_t *s, uint8_t cpu_pct, uint8_t mem_pct,
                       uint16_t active_conns, uint8_t error_rate_pct)
{
    s->metrics.cpu_pct        = cpu_pct;
    s->metrics.mem_pct        = mem_pct;
    s->metrics.active_conns   = active_conns;
    s->metrics.error_rate_pct = error_rate_pct;
    hc_update_temperature(s);
}

bool hc_rotate(hc_cluster_t *c)
{
    bool rotated = false;

    /* Step 1: Evict overheated inner servers */
    uint8_t max_evict = c->inner_count / 3;
    if (!max_evict) max_evict = 1;
    uint8_t evicted = 0;

    for (int8_t i = (int8_t)c->inner_count - 1; i >= 0; i--) {
        if (evicted >= max_evict) break;
        if (c->inner_count <= HC_MIN_INNER) break;

        hc_server_t *s = &c->servers[c->inner[i]];
        uint8_t threshold = (uint8_t)(HC_HEAT_THRESHOLD * s->weight);
        if (threshold > 100) threshold = 100;

        if (!s->metrics.healthy || s->temperature >= threshold) {
            hc_move_to_outer(c, (uint8_t)i);
            evicted++;
            rotated = true;
        }
    }

    /* Step 2: Pull coolest outer server into inner */
    while (c->outer_count > 0 &&
           c->inner_count < HC_MAX_INNER) {

        hc_server_t *coolest = &c->servers[c->outer[0]];
        if (coolest->temperature <= HC_COOL_THRESHOLD) {
            hc_move_to_inner(c, 0);
            rotated = true;
        } else {
            break;
        }
    }

    return rotated;
}

/*  Health report for httpd  */

static int hc_append(char *buf, int pos, int max, const char *s)
{
    while (*s && pos < max - 1) buf[pos++] = *s++;
    buf[pos] = 0;
    return pos;
}

static int hc_append_uint(char *buf, int pos, int max, uint32_t n)
{
    char tmp[16]; int i = 0;
    if (!n) { tmp[i++] = '0'; }
    while (n) { tmp[i++] = '0' + (n % 10); n /= 10; }
    while (i-- > 0 && pos < max - 1) buf[pos++] = tmp[i];
    buf[pos] = 0;
    return pos;
}

int hc_health_report(hc_cluster_t *c, char *buf, int max)
{
    int p = 0;
    p = hc_append(buf, p, max, "{\"inner\":[");

    for (uint8_t i = 0; i < c->inner_count; i++) {
        hc_server_t *s = &c->servers[c->inner[i]];
        if (i) p = hc_append(buf, p, max, ",");
        p = hc_append(buf, p, max, "{\"id\":\"");
        p = hc_append(buf, p, max, s->id);
        p = hc_append(buf, p, max, "\",\"temp\":");
        p = hc_append_uint(buf, p, max, s->temperature);
        p = hc_append(buf, p, max, ",\"avg_ms\":");
        p = hc_append_uint(buf, p, max, s->metrics.avg_latency_ms);
        p = hc_append(buf, p, max, ",\"conns\":");
        p = hc_append_uint(buf, p, max, s->metrics.active_conns);
        p = hc_append(buf, p, max, "}");
    }

    p = hc_append(buf, p, max, "],\"outer\":[");

    for (uint8_t i = 0; i < c->outer_count; i++) {
        hc_server_t *s = &c->servers[c->outer[i]];
        if (i) p = hc_append(buf, p, max, ",");
        p = hc_append(buf, p, max, "{\"id\":\"");
        p = hc_append(buf, p, max, s->id);
        p = hc_append(buf, p, max, "\",\"temp\":");
        p = hc_append_uint(buf, p, max, s->temperature);
        p = hc_append(buf, p, max, "}");
    }

    p = hc_append(buf, p, max, "],\"inner_count\":");
    p = hc_append_uint(buf, p, max, c->inner_count);
    p = hc_append(buf, p, max, ",\"outer_count\":");
    p = hc_append_uint(buf, p, max, c->outer_count);
    p = hc_append(buf, p, max, ",\"total_rotations\":");
    p = hc_append_uint(buf, p, max, (uint32_t)c->total_rotations);
    p = hc_append(buf, p, max, "}");

    return p;
}

/* Global cluster instance */
hc_cluster_t g_cluster;