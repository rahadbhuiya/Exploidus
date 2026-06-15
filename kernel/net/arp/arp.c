#include "arp.h"
#include "../tcp/tcp.h"
#include "../net.h"
#include "../../drivers/serial.h"
#include <string.h>


/*  ARP packet layout (RFC 826) */


#define ARP_HTYPE_ETH  1
#define ARP_PTYPE_IP4  0x0800
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

typedef struct __attribute__((packed)) {
    uint16_t   htype;       /* hardware type  — 1 = Ethernet */
    uint16_t   ptype;       /* protocol type  — 0x0800 = IPv4 */
    uint8_t    hlen;        /* hardware addr length — 6 */
    uint8_t    plen;        /* protocol addr length — 4 */
    uint16_t   op;          /* operation: 1=request, 2=reply */
    mac_addr_t sha;         /* sender hardware address */
    uint32_t   spa;         /* sender protocol address (net byte order) */
    mac_addr_t tha;         /* target hardware address */
    uint32_t   tpa;         /* target protocol address (net byte order) */
} arp_pkt_t;


/*  ARP cache */


#define ARP_CACHE_SIZE  16
#define ARP_TTL_TICKS   3000   /* ~30 seconds at 100 Hz */

typedef struct {
    ip4_t      ip;
    mac_addr_t mac;
    uint32_t   ttl;
    bool       valid;
    bool       permanent;
} arp_entry_t;

static arp_entry_t g_cache[ARP_CACHE_SIZE];
extern uint64_t g_uptime_ticks;

void arp_init(void)
{
    memset(g_cache, 0, sizeof(g_cache));
}

static void arp_cache_update(ip4_t ip, mac_addr_t mac)
{
    /* Update existing entry */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].valid && g_cache[i].ip == ip) {
            g_cache[i].mac = mac;
            g_cache[i].ttl = g_uptime_ticks + ARP_TTL_TICKS;
            return;
        }
    }

    /* Find free or oldest slot */
    int target = 0;
    uint32_t oldest_ttl = 0xFFFFFFFF;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_cache[i].valid) { target = i; break; }
        if (g_cache[i].ttl < oldest_ttl) {
            oldest_ttl = g_cache[i].ttl;
            target = i;
        }
    }

    g_cache[target].ip    = ip;
    g_cache[target].mac   = mac;
    g_cache[target].ttl   = g_uptime_ticks + ARP_TTL_TICKS;
    g_cache[target].valid = true;
}

/* Pre-populate QEMU gateway after arp_cache_update is defined */
void arp_preload_qemu_gateway(void)
{
    mac_addr_t gw_mac = {{ 0xde, 0x7c, 0x85, 0xe3, 0x74, 0x90 }};
    arp_cache_update(IP4(10, 0, 2, 2), gw_mac);
    /* Mark as permanent — never expires */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].valid && g_cache[i].ip == IP4(10, 0, 2, 2))
            g_cache[i].permanent = true;
    }
}

bool arp_resolve(netif_t *iface, ip4_t ip, mac_addr_t *out)
{
    /* Broadcast resolves to broadcast MAC */
    if (ip == IP4_BROADCAST) {
        for (int i = 0; i < 6; i++) out->b[i] = 0xFF;
        return true;
    }

    /* Same subnet — look up in cache */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].valid && g_cache[i].ip == ip &&
            (g_cache[i].permanent || g_cache[i].ttl > g_uptime_ticks))
        {
            *out = g_cache[i].mac;
            return true;
        }
    }

    /* Send ARP request */
    netbuf_t *buf = netbuf_alloc();
    if (!buf) return false;

    arp_pkt_t *pkt = (arp_pkt_t *)buf->data;
    pkt->htype = htons(ARP_HTYPE_ETH);
    pkt->ptype = htons(ARP_PTYPE_IP4);
    pkt->hlen  = 6;
    pkt->plen  = 4;
    pkt->op    = htons(ARP_OP_REQUEST);
    pkt->sha   = iface->mac;
    pkt->spa   = htonl(iface->ip);
    memset(&pkt->tha, 0, sizeof(mac_addr_t));
    pkt->tpa   = htonl(ip);
    buf->len   = (uint16_t)sizeof(arp_pkt_t);

    /* Prepend Ethernet header */
    eth_hdr_t *eth = (eth_hdr_t *)netbuf_push(buf, sizeof(eth_hdr_t));
    if (!eth) { netbuf_free(buf); return false; }
    for (int i = 0; i < 6; i++) eth->dst.b[i] = 0xFF;
    eth->src  = iface->mac;
    eth->type = htons(ETHERTYPE_ARP);

    iface->tx(iface, buf);
    netbuf_free(buf);
    return false;  /* not resolved yet — caller retries */
}

void arp_input(netif_t *iface, netbuf_t *buf)
{
    if (buf->len < sizeof(arp_pkt_t)) return;

    arp_pkt_t *pkt = (arp_pkt_t *)buf->data;

    if (ntohs(pkt->htype) != ARP_HTYPE_ETH) return;
    if (ntohs(pkt->ptype) != ARP_PTYPE_IP4) return;

    ip4_t sender_ip = ntohl(pkt->spa);
    arp_cache_update(sender_ip, pkt->sha);
    tcp_flush_pending_synacks(iface, sender_ip);

    /* Only respond to requests targeting our IP */
    if (ntohs(pkt->op) != ARP_OP_REQUEST) return;
    if (ntohl(pkt->tpa) != iface->ip) return;

    serial_print("[ARP] Replying to request for our IP\n");

    /* Send ARP reply */
    netbuf_t *reply = netbuf_alloc();
    if (!reply) return;

    arp_pkt_t *r = (arp_pkt_t *)reply->data;
    r->htype = htons(ARP_HTYPE_ETH);
    r->ptype = htons(ARP_PTYPE_IP4);
    r->hlen  = 6;
    r->plen  = 4;
    r->op    = htons(ARP_OP_REPLY);
    r->sha   = iface->mac;
    r->spa   = htonl(iface->ip);
    r->tha   = pkt->sha;
    r->tpa   = pkt->spa;
    reply->len = (uint16_t)sizeof(arp_pkt_t);

    eth_hdr_t *eth = (eth_hdr_t *)netbuf_push(reply, sizeof(eth_hdr_t));
    if (!eth) { netbuf_free(reply); return; }
    eth->dst  = pkt->sha;
    eth->src  = iface->mac;
    eth->type = htons(ETHERTYPE_ARP);

    iface->tx(iface, reply);
    netbuf_free(reply);
}

void arp_announce(netif_t *iface)
{
    /* Gratuitous ARP — announce our IP to the network */
    mac_addr_t dummy;
    arp_resolve(iface, iface->ip, &dummy);
}