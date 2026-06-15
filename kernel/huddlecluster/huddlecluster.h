#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * huddlecluster.h — Penguin-inspired Load Balancer (Kernel Port)
 *
 * Inner Ring: active servers handling requests (deque, round-robin)
 * Outer Ring: resting servers cooling down (sorted by temperature)
 *
 * Temperature = EMA(cpu*0.10 + mem*0.05 + conn*0.10 + latency_anomaly*0.70 + err*0.05)
 *
 * Rotation:
 *   - Overheated inner server  → outer ring
 *   - Coolest outer server     → inner ring
 *
 * Author: Rahad Bhuiya (kernel C port of HuddleCluster v1.3.0)
 */

/*  Config  */
#define HC_MAX_SERVERS       32
#define HC_MAX_INNER         8
#define HC_MIN_INNER         2
#define HC_HEAT_THRESHOLD    55   /* 0-100 scale */
#define HC_COOL_THRESHOLD    30
#define HC_EMA_ALPHA_PCT     60   /* 60% = 0.60 EMA alpha */
#define HC_LATENCY_WINDOW    10   /* rolling window size */
#define HC_ROTATION_LOG      64

/*  Server position  */
#define HC_INNER  0
#define HC_OUTER  1

/*  Server metrics  */
typedef struct {
    uint8_t  cpu_pct;           /* 0-100 */
    uint8_t  mem_pct;           /* 0-100 */
    uint16_t active_conns;
    uint32_t avg_latency_ms;    /* rolling window average */
    uint8_t  error_rate_pct;    /* 0-100 */
    bool     healthy;
    uint32_t latency_window[HC_LATENCY_WINDOW];
    uint8_t  window_head;
    uint8_t  window_count;
    uint32_t anomaly_score;     /* 0-100, relative vs cluster avg */
} hc_metrics_t;

/*  Server  */
typedef struct {
    char     id[16];
    uint32_t ip;                /* network byte order */
    uint16_t port;
    uint8_t  position;          /* HC_INNER or HC_OUTER */
    uint8_t  weight;            /* 1 = normal, 2 = double capacity */
    uint8_t  temperature;       /* 0-100, EMA smoothed */
    uint32_t rotation_count;
    uint64_t total_inner_ticks;
    uint64_t total_outer_ticks;
    uint64_t last_rotated_tick;
    bool     active;
    hc_metrics_t metrics;
} hc_server_t;

/*  Rotation event  */
typedef struct {
    char    server_id[16];
    uint8_t direction;          /* 0=inner→outer, 1=outer→inner */
    uint8_t reason;             /* 0=overheated, 1=cooled, 2=health */
    uint8_t temperature;
} hc_rotation_event_t;

/*  Cluster  */
typedef struct {
    hc_server_t   servers[HC_MAX_SERVERS];
    uint8_t       server_count;

    /* Inner ring — circular, round-robin */
    uint8_t       inner[HC_MAX_INNER];   /* indices into servers[] */
    uint8_t       inner_count;
    uint8_t       inner_head;            /* next server to return */

    /* Outer ring — sorted by temperature (coolest first) */
    uint8_t       outer[HC_MAX_SERVERS];
    uint8_t       outer_count;

    /* Rotation log */
    hc_rotation_event_t log[HC_ROTATION_LOG];
    uint8_t       log_head;

    uint64_t      total_rotations;
} hc_cluster_t;

/*  Public API  */

/* Initialize cluster */
void hc_init(hc_cluster_t *c);

/* Add server — starts in outer ring */
bool hc_add_server(hc_cluster_t *c, const char *id,
                   uint32_t ip, uint16_t port, uint8_t weight);

/* Get next server for request (round-robin from inner ring) */
hc_server_t *hc_get_server(hc_cluster_t *c);

/* Record latency after a request — updates temperature */
void hc_record_latency(hc_cluster_t *c, hc_server_t *s, uint32_t latency_ms);

/* Update metrics externally */
void hc_update_metrics(hc_server_t *s, uint8_t cpu_pct, uint8_t mem_pct,
                       uint16_t active_conns, uint8_t error_rate_pct);

/* Run one rotation cycle — call from scheduler tick or timer */
bool hc_rotate(hc_cluster_t *c);

/* Health report to buffer — for httpd /huddlecluster endpoint */
int  hc_health_report(hc_cluster_t *c, char *buf, int max);