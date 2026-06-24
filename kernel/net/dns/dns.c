#include "dns.h"
#include "../udp/udp.h"
#include "../net.h"
#include "../../drivers/serial.h"
#include <string.h>

/*
 * Minimal RFC 1035 DNS resolver.
 *
 * Only handles A-record (IPv4) queries and the simplest response
 * format (no compression pointers in the answer, first A record wins).
 * That covers every real-world QEMU user-networking response for
 * simple hostnames.
 *
 * QEMU user-networking DNS is at 10.0.2.3:53 by default.
 */

#define DNS_SERVER    IP4(10,0,2,3)
#define DNS_PORT      53
#define DNS_SRC_PORT  5353   /* fixed ephemeral — simpler than allocating one */
#define DNS_TIMEOUT   200000 /* poll iterations before giving up */

#define DNS_TYPE_A    1
#define DNS_CLASS_IN  1

/* Single shared response slot — only one query at a time. */
static volatile ip4_t  g_dns_result  = 0;
static volatile int    g_dns_done    = 0;
static volatile uint16_t g_dns_txid = 0;

/* -----------------------------------------------------------------------
 * Encode a dotted hostname into DNS wire format (length-prefixed labels).
 * Returns the number of bytes written into buf.
 * ----------------------------------------------------------------------- */
static uint16_t dns_encode_name(const char *hostname, uint8_t *buf)
{
    uint16_t pos = 0;
    const char *label = hostname;
    while (*label) {
        const char *dot = label;
        while (*dot && *dot != '.') dot++;
        uint8_t len = (uint8_t)(dot - label);
        buf[pos++] = len;
        for (uint8_t i = 0; i < len; i++) buf[pos++] = (uint8_t)label[i];
        label = dot;
        if (*label == '.') label++;
    }
    buf[pos++] = 0;  /* root label */
    return pos;
}

/* -----------------------------------------------------------------------
 * udp_bind callback — called from the interrupt/poll path when a UDP
 * packet arrives on DNS_SRC_PORT.
 * ----------------------------------------------------------------------- */
static void dns_recv_cb(netif_t *iface, ip4_t src_ip, uint16_t src_port,
                        const uint8_t *data, uint16_t len)
{
    (void)iface; (void)src_ip; (void)src_port;

    /* Minimum DNS header: 12 bytes */
    if (len < 12) return;

    /* Check transaction ID matches */
    uint16_t txid = (uint16_t)((data[0] << 8) | data[1]);
    if (txid != g_dns_txid) return;

    /* QR bit must be 1 (response), RCODE must be 0 (no error) */
    uint8_t flags1 = data[2];
    uint8_t rcode  = data[3] & 0x0F;
    if (!(flags1 & 0x80)) return;  /* not a response */
    if (rcode != 0) { g_dns_done = 1; return; }

    uint16_t qdcount = (uint16_t)((data[4]  << 8) | data[5]);
    uint16_t ancount = (uint16_t)((data[6]  << 8) | data[7]);
    (void)qdcount;
    if (ancount == 0) { g_dns_done = 1; return; }

    /* Skip the question section.  DNS name compression is possible here:
     * scan past the QNAME (zero-terminated or pointer), then 4 bytes
     * (QTYPE + QCLASS). */
    uint16_t off = 12;
    /* Skip QNAME in question section */
    while (off < len) {
        uint8_t llen = data[off];
        if (llen == 0) { off++; break; }
        if ((llen & 0xC0) == 0xC0) { off += 2; break; }  /* pointer */
        off += 1 + llen;
    }
    off += 4;  /* skip QTYPE + QCLASS */

    /* Walk answer records looking for an A record */
    for (uint16_t i = 0; i < ancount && off < len; i++) {
        /* Skip NAME (may be a pointer) */
        if (off >= len) break;
        uint8_t first = data[off];
        if ((first & 0xC0) == 0xC0) {
            off += 2;   /* compression pointer */
        } else {
            while (off < len) {
                uint8_t l = data[off];
                if (l == 0) { off++; break; }
                if ((l & 0xC0) == 0xC0) { off += 2; break; }
                off += 1 + l;
            }
        }
        if (off + 10 > len) break;

        uint16_t rtype  = (uint16_t)((data[off] << 8) | data[off+1]);
        uint16_t rclass = (uint16_t)((data[off+2] << 8) | data[off+3]);
        /* skip TTL (4 bytes) */
        uint16_t rdlen  = (uint16_t)((data[off+8] << 8) | data[off+9]);
        off += 10;

        if (rtype == DNS_TYPE_A && rclass == DNS_CLASS_IN && rdlen == 4
            && off + 4 <= len)
        {
            ip4_t ip = ((ip4_t)data[off]   << 24) |
                       ((ip4_t)data[off+1] << 16) |
                       ((ip4_t)data[off+2] <<  8) |
                        (ip4_t)data[off+3];
            g_dns_result = ip;
            g_dns_done   = 1;
            return;
        }
        off += rdlen;
    }

    g_dns_done = 1;  /* no A record found, but we did get a response */
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
ip4_t dns_resolve(const char *hostname)
{
    netif_t *iface = netif_default();
    if (!iface) return 0;

    g_dns_result = 0;
    g_dns_done   = 0;

    /* Simple pseudo-random transaction ID from hostname bytes */
    uint16_t txid = 0x1A2B;
    for (const char *s = hostname; *s; s++) txid = (uint16_t)(txid * 31 + (uint8_t)*s);
    g_dns_txid = txid;

    /* Register UDP listener (idempotent: re-bind if already bound) */
    udp_bind(DNS_SRC_PORT, dns_recv_cb);

    /* Build DNS query packet */
    uint8_t pkt[280];
    memset(pkt, 0, sizeof(pkt));
    uint16_t pos = 0;

    /* Header */
    pkt[pos++] = (uint8_t)(txid >> 8);
    pkt[pos++] = (uint8_t)(txid);
    pkt[pos++] = 0x01;  /* QR=0 RD=1 */
    pkt[pos++] = 0x00;
    pkt[pos++] = 0x00; pkt[pos++] = 0x01;  /* QDCOUNT=1 */
    pkt[pos++] = 0x00; pkt[pos++] = 0x00;  /* ANCOUNT=0 */
    pkt[pos++] = 0x00; pkt[pos++] = 0x00;  /* NSCOUNT=0 */
    pkt[pos++] = 0x00; pkt[pos++] = 0x00;  /* ARCOUNT=0 */

    pos += dns_encode_name(hostname, pkt + pos);

    /* QTYPE=A QCLASS=IN */
    pkt[pos++] = 0x00; pkt[pos++] = 0x01;
    pkt[pos++] = 0x00; pkt[pos++] = 0x01;

    udp_send(iface, DNS_SERVER, DNS_SRC_PORT, DNS_PORT, pkt, pos);

    extern void net_poll(void);
    extern void sched_yield(void);

    for (int i = 0; i < DNS_TIMEOUT && !g_dns_done; i++) {
        net_poll();
        sched_yield();
    }

    return g_dns_result;
}