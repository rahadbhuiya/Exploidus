#include "tcp.h"
#include "../arp/arp.h"
#include "../ip/ip.h"
#include "../net.h"
#include "../../drivers/serial.h"
#include "../../mm/kmalloc.h"
#include "../../cnsl/cnsl.h"
#include <string.h>

#define MAX_TCP_CONNS  32
extern uint64_t g_uptime_ticks;
#define ISS_BASE       0x12345678   /* fallback if RDRAND unavailable */

static tcp_conn_t g_conns[MAX_TCP_CONNS];
static uint32_t   g_iss = ISS_BASE;

/* RDRAND helper — returns random uint32, falls back to counter */
static uint32_t tcp_rand32(void)
{
    uint64_t val = 0;
    uint8_t  ok  = 0;
    for (int i = 0; i < 10; i++) {
        uint32_t cpuid_ecx = 0; __asm__ volatile("cpuid" : "=c"(cpuid_ecx) : "a"(1) : "ebx","edx"); if (!(cpuid_ecx & (1<<30))) { static uint32_t s = 0xDEADBEEF; s ^= s<<13; s ^= s>>17; s ^= s<<5; return s; } __asm__ volatile ("rdrand %0\n setc %1\n"
                          : "=r"(val), "=qm"(ok));
        if (ok) return (uint32_t)val;
    }
    /* Fallback: increment by a large prime */
    static uint32_t s_ctr = 0xDEADBEEF;
    s_ctr += 2654435761u;  /* Knuth multiplicative hash */
    return s_ctr;
}


/*  Internal helpers  */

static tcp_conn_t *conn_alloc(void)
{
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (g_conns[i].state == TCP_CLOSED) {
            memset(&g_conns[i], 0, sizeof(tcp_conn_t));
            g_conns[i].rcv_wnd = TCP_BUF_SIZE;
            return &g_conns[i];
        }
    }
    return NULL;
}

static tcp_conn_t *conn_find(ip4_t remote_ip, uint16_t remote_port,
                              uint16_t local_port)
{
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        tcp_conn_t *c = &g_conns[i];
        if (c->state == TCP_CLOSED) continue;
        if (c->local_port  == local_port  &&
            c->remote_port == remote_port &&
            c->remote_ip   == remote_ip)
            return c;
    }
    return NULL;
}

static tcp_conn_t *listener_find(uint16_t local_port)
{
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (g_conns[i].state      == TCP_LISTEN &&
            g_conns[i].local_port == local_port)
            return &g_conns[i];
    }
    return NULL;
}

/* rx ring buffer helpers */
static uint16_t rx_available(tcp_conn_t *c)
{
    return (uint16_t)((c->rx_head - c->rx_tail) & (TCP_BUF_SIZE - 1));
}
static void rx_push(tcp_conn_t *c, const uint8_t *data, uint16_t len)
{
    /* Never write more than free space in the ring buffer */
    uint16_t free_space = TCP_BUF_SIZE - rx_available(c);
    if (len > free_space) len = free_space;

    for (uint16_t i = 0; i < len; i++) {
        c->rx_buf[c->rx_head & (TCP_BUF_SIZE - 1)] = data[i];
        c->rx_head++;
    }
    /* Advertise updated receive window */
    c->rcv_wnd = TCP_BUF_SIZE - rx_available(c);
}


/*  Send a TCP segment      */


static bool tcp_send_segment(netif_t *iface, tcp_conn_t *conn,
                              uint8_t flags,
                              const void *data, uint16_t data_len)
{
    netbuf_t *buf = netbuf_alloc();
    if (!buf) return false;

    /* Copy payload */
    if (data && data_len) {
        memcpy(buf->data, data, data_len);
        buf->len = data_len;
    } else {
        buf->len = 0;
    }

    /* Build TCP options into a local buffer, then push once */
    uint8_t opt_len = 0;

    if (flags & TCP_SYN) {
        /* Push 16 bytes: MSS(4)+NOP+NOP+TS(10) */
        uint8_t full_opt[16];
        /* MSS */
        full_opt[0] = 2;  full_opt[1] = 4;
        full_opt[2] = (1460 >> 8) & 0xFF;
        full_opt[3] = 1460 & 0xFF;
        /* NOP NOP */
        full_opt[4] = 1; full_opt[5] = 1;
        /* TS */
        uint32_t tsval = (uint32_t)g_uptime_ticks;
        uint32_t tsecr = conn->ts_recent;
        full_opt[6]  = 8; full_opt[7] = 10;
        full_opt[8]  = (tsval >> 24) & 0xFF;
        full_opt[9]  = (tsval >> 16) & 0xFF;
        full_opt[10] = (tsval >>  8) & 0xFF;
        full_opt[11] = (tsval      ) & 0xFF;
        full_opt[12] = (tsecr >> 24) & 0xFF;
        full_opt[13] = (tsecr >> 16) & 0xFF;
        full_opt[14] = (tsecr >>  8) & 0xFF;
        full_opt[15] = (tsecr      ) & 0xFF;
        opt_len = 16;

        uint8_t *opt = (uint8_t *)netbuf_push(buf, opt_len);
        if (opt) memcpy(opt, full_opt, opt_len);
        else opt_len = 0;
    }

    /* Prepend TCP header */
    tcp_hdr_t *hdr = (tcp_hdr_t *)netbuf_push(buf, TCP_HDR_LEN);
    if (!hdr) { netbuf_free(buf); return false; }

    hdr->src_port    = htons(conn->local_port);
    hdr->dst_port    = htons(conn->remote_port);
    hdr->seq         = htonl(conn->snd_nxt);
    hdr->ack         = (flags & TCP_ACK) ? htonl(conn->rcv_nxt) : 0;
    hdr->data_offset = (uint8_t)(((TCP_HDR_LEN + opt_len) / 4) << 4);
    hdr->flags       = flags;
    hdr->window      = htons(conn->rcv_wnd);
    hdr->checksum    = 0;
    hdr->urgent      = 0;

    hdr->checksum = htons(inet_cksum_pseudo(conn->local_ip, conn->remote_ip,
                                      IP_PROTO_TCP,
                                      (uint16_t)buf->len,
                                      buf->data, buf->len));

    if (flags & (TCP_SYN | TCP_FIN)) conn->snd_nxt++;
    conn->snd_nxt += data_len;

    bool ok = ip4_output(iface, conn->remote_ip, IP_PROTO_TCP, buf);
    netbuf_free(buf);
    return ok;
}


/*  tcp_flush_pending_synacks — called from arp_input after ARP reply  */

void tcp_flush_pending_synacks(netif_t *iface, ip4_t resolved_ip)
{
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        tcp_conn_t *c = &g_conns[i];
        if (c->state == TCP_SYN_RECV &&
            c->pending_synack &&
            c->remote_ip == resolved_ip)
        {
            c->pending_synack = 0;
            tcp_send_segment(iface, c, TCP_SYN | TCP_ACK, NULL, 0);
        }
    }
}


/*  tcp_init   */


void tcp_init(void)
{
    memset(g_conns, 0, sizeof(g_conns));
    g_iss = tcp_rand32();  /* randomise initial sequence number */
}


/*  tcp_input — receive a segment from the IP layer     */


void tcp_input(netif_t *iface, netbuf_t *buf, ip4_t src, ip4_t dst)
{
    (void)dst;
    if (buf->len < TCP_HDR_LEN) return;

    /* Skip checksum verification — QEMU NAT may use checksum offloading */

    tcp_hdr_t *hdr     = (tcp_hdr_t *)buf->data;
    uint16_t   src_port = ntohs(hdr->src_port);
    uint16_t   dst_port = ntohs(hdr->dst_port);
    uint8_t    hdr_len  = (uint8_t)((hdr->data_offset >> 4) * 4);
    uint32_t   seq      = ntohl(hdr->seq);
    uint32_t   ack_num  = ntohl(hdr->ack);
    uint8_t    flags    = hdr->flags;

    /* CNSL: Honeypot port detection — after flags is available */
    if (flags & TCP_SYN) {
        static const uint16_t HONEYPOT_PORTS[] = {
            23, 3389, 6379, 27017, 5900, 445,
        };
        for (int _i = 0; _i < 6; _i++) {
            if (dst_port == HONEYPOT_PORTS[_i]) {
                serial_print("[CNSL] Honeypot port hit\n");
                cnsl_ingest(src, CNSL_KIND_FW_HONEYPOT_PORT);
                break;
            }
        }
    }

    if (hdr_len < TCP_HDR_LEN || hdr_len > buf->len) return;

    const uint8_t *payload     = buf->data + hdr_len;
    uint16_t       payload_len = (uint16_t)(buf->len - hdr_len);

    /* Find existing connection */
    tcp_conn_t *conn = conn_find(src, src_port, dst_port);

    if (!conn) {
        /* Check for a listener on this port */
        tcp_conn_t *listener = listener_find(dst_port);
        if (!listener || !(flags & TCP_SYN)) {
            return; /* silently drop */
        }

        /* Three-way handshake — SYN received on listening socket */
        /* Parse peer timestamp from SYN options */
        uint32_t peer_ts = 0;
        if (hdr_len > TCP_HDR_LEN) {
            const uint8_t *op = buf->data + TCP_HDR_LEN;
            const uint8_t *op_end = buf->data + hdr_len;
            while (op < op_end) {
                if (op[0] == 0) break;          /* EOL */
                if (op[0] == 1) { op++; continue; } /* NOP */
                if (op + 1 >= op_end) break;
                uint8_t olen = op[1];
                if (olen < 2 || op + olen > op_end) break;
                if (op[0] == 8 && olen == 10) { /* Timestamp */
                    peer_ts = ((uint32_t)op[2] << 24) | ((uint32_t)op[3] << 16) |
                              ((uint32_t)op[4] <<  8) |  (uint32_t)op[5];
                }
                op += olen;
            }
        }

        conn = conn_alloc();
        if (!conn) return;

        conn->state       = TCP_SYN_RECV;
        conn->local_ip    = dst;
        conn->remote_ip   = src;
        conn->local_port  = dst_port;
        conn->remote_port = src_port;
        conn->snd_nxt     = g_iss++;
        conn->rcv_nxt     = seq + 1;
        conn->snd_wnd     = ntohs(hdr->window);
        conn->ts_recent   = peer_ts;  /* echo back in SYN-ACK */

        /* Chain onto listener's accept queue via next pointer */
        conn->next = listener->next;
        listener->next = conn;

        /* Send SYN-ACK only if MAC already in ARP cache,
         * otherwise set pending_synack for arp_input to flush. */
        {
            mac_addr_t mac;
            if (arp_resolve(iface, src, &mac)) {
                tcp_send_segment(iface, conn, TCP_SYN | TCP_ACK, NULL, 0);
            } else {
                conn->pending_synack = 1;
            }
        }
        return;
    }

    /* Update peer window */
    conn->snd_wnd = ntohs(hdr->window);

    switch (conn->state) {

    case TCP_SYN_RECV:
        if ((flags & TCP_ACK) && ack_num == conn->snd_nxt) {
            conn->state = TCP_ESTABLISHED;
            serial_print("[TCP] Connection established port=");
            serial_printhex((uint64_t)conn->local_port);
            serial_print("\n");
        } else if (flags & TCP_SYN) {
            /* Client retried SYN — resend SYN-ACK with same initial seq */
            conn->rcv_nxt = seq + 1;
            uint32_t init_seq = conn->snd_nxt - 1; /* snd_nxt was incremented after first SYN-ACK */
            conn->snd_nxt = init_seq;
            tcp_send_segment(iface, conn, TCP_SYN | TCP_ACK, NULL, 0);
            conn->snd_nxt = init_seq + 1; /* keep it consistent */
        }
        break;

    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            conn->rcv_nxt = seq + 1;
            conn->snd_una = ack_num;
            conn->state   = TCP_ESTABLISHED;
            tcp_send_segment(iface, conn, TCP_ACK, NULL, 0);
            serial_print("[TCP] Connected to remote\n");
        }
        break;

    case TCP_ESTABLISHED:
        /* Process incoming data */
        if (payload_len > 0 && seq == conn->rcv_nxt) {
            rx_push(conn, payload, payload_len);
            conn->rcv_nxt += payload_len;
            tcp_send_segment(iface, conn, TCP_ACK, NULL, 0);
        } else if (payload_len > 0) {
            serial_print(" expected="); serial_printhex((uint64_t)conn->rcv_nxt); serial_print("\n");
        }

        /* Process ACK */
        if (flags & TCP_ACK)
            conn->snd_una = ack_num;

        /* Peer initiated close */
        if (flags & TCP_FIN) {
            conn->rcv_nxt++;
            conn->state = TCP_CLOSE_WAIT;
            tcp_send_segment(iface, conn, TCP_ACK, NULL, 0);
        }
        break;

    case TCP_FIN_WAIT1:
        if (flags & TCP_ACK) conn->state = TCP_FIN_WAIT2;
        if (flags & TCP_FIN) {
            conn->rcv_nxt++;
            tcp_send_segment(iface, conn, TCP_ACK, NULL, 0);
            conn->state = TCP_TIME_WAIT;
        }
        break;

    case TCP_FIN_WAIT2:
        if (flags & TCP_FIN) {
            conn->rcv_nxt++;
            tcp_send_segment(iface, conn, TCP_ACK, NULL, 0);
            conn->state = TCP_TIME_WAIT;
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_ACK) conn->state = TCP_CLOSED;
        break;

    case TCP_TIME_WAIT:
        /* Wait 2MSL then close — simplified: close immediately */
        conn->state = TCP_CLOSED;
        break;

    default:
        break;
    }
}


/*  Public API */


tcp_conn_t *tcp_connect(netif_t *iface, ip4_t dst_ip,
                         uint16_t dst_port, uint16_t src_port)
{
    tcp_conn_t *conn = conn_alloc();
    if (!conn) return NULL;

    /* Pick an unused ephemeral port if none specified */
    if (!src_port) {
        static uint16_t s_next_port = 49152;
        for (int tries = 0; tries < 16384; tries++) {
            uint16_t candidate = s_next_port++;
            if (s_next_port == 0) s_next_port = 49152;
            bool in_use = false;
            for (int i = 0; i < MAX_TCP_CONNS; i++) {
                if (g_conns[i].state != TCP_CLOSED &&
                    g_conns[i].local_port == candidate) {
                    in_use = true; break;
                }
            }
            if (!in_use) { src_port = candidate; break; }
        }
        if (!src_port) { conn->state = TCP_CLOSED; return NULL; }
    }

    conn->state       = TCP_SYN_SENT;
    conn->local_ip    = iface->ip;
    conn->remote_ip   = dst_ip;
    conn->local_port  = src_port;
    conn->remote_port = dst_port;
    conn->snd_nxt     = g_iss++;
    conn->rcv_wnd     = TCP_BUF_SIZE;

    tcp_send_segment(iface, conn, TCP_SYN, NULL, 0);
    return conn;
}

tcp_conn_t *tcp_listen(uint16_t port)
{
    tcp_conn_t *conn = conn_alloc();
    if (!conn) return NULL;
    conn->state      = TCP_LISTEN;
    conn->local_port = port;
    return conn;
}

tcp_conn_t *tcp_accept(tcp_conn_t *listener)
{
    if (!listener || listener->state != TCP_LISTEN) return NULL;
    /* Search chain for ESTABLISHED or CLOSE_WAIT connection */
    tcp_conn_t *prev = NULL;
    tcp_conn_t *conn = listener->next;
    while (conn) {
        if (conn->state == TCP_ESTABLISHED ||
            conn->state == TCP_CLOSE_WAIT) {
            /* Remove from accept queue */
            if (prev) prev->next = conn->next;
            else      listener->next = conn->next;
            conn->next = NULL;
            return conn;
        }
        prev = conn;
        conn = conn->next;
    }
    return NULL;
}

int16_t tcp_send(tcp_conn_t *conn, const void *data, uint16_t len)
{
    if (!conn || conn->state != TCP_ESTABLISHED) return -1;
    if (!len) return 0;

    /* Loopback fast path — write directly to partner's rx_buf */
    if (conn->partner) {
        tcp_conn_t *p = conn->partner;
        uint16_t written = 0;
        const uint8_t *src = (const uint8_t *)data;
        while (written < len) {
            uint16_t space = (uint16_t)(TCP_BUF_SIZE - ((p->rx_head - p->rx_tail) & (TCP_BUF_SIZE - 1)));
            if (!space) break;
            p->rx_buf[p->rx_head & (TCP_BUF_SIZE - 1)] = src[written];
            p->rx_head++;
            written++;
        }
        return (int16_t)written;
    }

    netif_t *iface = netif_default();
    if (!iface) return -1;

    /* Split into MSS-sized chunks if needed (MSS = 1460 for Ethernet) */
    uint16_t mss  = 1460;
    uint16_t sent = 0;

    while (sent < len) {
        uint16_t chunk = len - sent;
        if (chunk > mss) chunk = mss;

        if (!tcp_send_segment(iface, conn, TCP_PSH | TCP_ACK,
                              (const uint8_t *)data + sent, chunk))
            break;
        sent += chunk;
    }
    return (int16_t)sent;
}

int16_t tcp_recv(tcp_conn_t *conn, void *buf, uint16_t len)
{
    if (!conn) return -1;

    uint16_t avail = rx_available(conn);
    if (!avail) return 0;

    uint16_t n = avail < len ? avail : len;
    uint8_t *p = (uint8_t *)buf;

    for (uint16_t i = 0; i < n; i++) {
        p[i] = conn->rx_buf[conn->rx_tail & (TCP_BUF_SIZE - 1)];
        conn->rx_tail++;
    }
    return (int16_t)n;
}

void tcp_close(tcp_conn_t *conn)
{
    if (!conn || conn->state == TCP_CLOSED) return;

    netif_t *iface = netif_default();
    if (conn->state == TCP_ESTABLISHED || conn->state == TCP_CLOSE_WAIT) {
        conn->state = (conn->state == TCP_ESTABLISHED)
                    ? TCP_FIN_WAIT1 : TCP_LAST_ACK;
        if (iface) tcp_send_segment(iface, conn, TCP_FIN | TCP_ACK, NULL, 0);
    } else {
        conn->state = TCP_CLOSED;
    }
}

bool tcp_is_connected(tcp_conn_t *conn)
{
    return conn && conn->state == TCP_ESTABLISHED;
}

/*  Loopback support  */

tcp_conn_t *tcp_listen_find(uint16_t port)
{
    return listener_find(port);
}

tcp_conn_t *tcp_conn_alloc_for_loopback(tcp_conn_t *listener, uint16_t client_port, ip4_t client_ip)
{
    tcp_conn_t *conn = conn_alloc();
    if (!conn) return NULL;

    conn->state       = TCP_ESTABLISHED;
    conn->local_port  = listener->local_port;
    conn->remote_port = client_port;
    conn->remote_ip   = client_ip;
    conn->snd_nxt     = g_iss++;
    conn->rcv_nxt     = g_iss;

    /* Link to listener queue */
    conn->next = listener->next;
    listener->next = conn;

    return conn;
}

tcp_conn_t *tcp_conn_alloc_for_loopback_client(uint16_t server_port, uint16_t client_port, ip4_t server_ip)
{
    tcp_conn_t *conn = conn_alloc();
    if (!conn) return NULL;

    conn->state       = TCP_ESTABLISHED;
    conn->local_port  = client_port;
    conn->remote_port = server_port;
    conn->remote_ip   = server_ip;
    conn->snd_nxt     = g_iss;
    conn->rcv_nxt     = g_iss++;

    return conn;
}