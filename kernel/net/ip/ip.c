#include "ip.h"
#include "../arp/arp.h"
#include "../net.h"
#include "../../drivers/serial.h"
#include "../../mm/kmalloc.h"
#include "../../cnsl/cnsl.h"
#include <string.h>

static uint16_t g_ip_id = 0;

/* Forward declarations for upper layers */
extern void icmp_input(netif_t *iface, netbuf_t *buf,
                       ip4_t src, ip4_t dst);
extern void tcp_input(netif_t *iface, netbuf_t *buf,
                      ip4_t src, ip4_t dst);
extern void udp_input(netif_t *iface, netbuf_t *buf,
                      ip4_t src, ip4_t dst);


/*  IP fragment reassembly                                              */
/*                                                                       */
/*  RFC 791 §3.2. Each slot holds one partially-reassembled datagram.  */
/*  Key: (src, dst, proto, id).  Payload max: 65535 bytes.            */
/*  Slots are purged immediately upon completion.                       */
/*  Stale slots (no last-fragment arrived) are silently dropped when   */
/*  a new datagram needs the slot (LRU-replace).                       */


#define FRAG_MAX_SLOTS    4
#define FRAG_MAX_PAYLOAD  65535

typedef struct {
    ip4_t    src, dst;
    uint8_t  proto;
    uint16_t ip_id;
    uint8_t *buf;         /* kmalloc'd reassembly buffer */
    uint16_t total_len;   /* set when MF=0 fragment arrives */
    uint16_t filled;      /* bytes we've deposited so far */
    bool     have_last;   /* did we receive the last fragment? */
    bool     active;
    uint32_t age;         /* monotonic counter for LRU eviction */
} frag_slot_t;

static frag_slot_t g_frags[FRAG_MAX_SLOTS];
static uint32_t    g_frag_age = 0;  /* global counter */

/* IP flags/offset field bit masks (network byte order already decoded) */
#define IP_FLAG_MF   0x2000u   /* More Fragments */
#define IP_FLAG_DF   0x4000u   /* Don't Fragment */
#define IP_FRAG_MASK 0x1FFFu   /* Fragment offset (in 8-byte units) */

static frag_slot_t *frag_find(ip4_t src, ip4_t dst,
                               uint8_t proto, uint16_t id)
{
    for (int i = 0; i < FRAG_MAX_SLOTS; i++) {
        frag_slot_t *s = &g_frags[i];
        if (s->active && s->src == src && s->dst == dst &&
            s->proto == proto && s->ip_id == id)
            return s;
    }
    return NULL;
}

static frag_slot_t *frag_alloc(ip4_t src, ip4_t dst,
                                uint8_t proto, uint16_t id)
{
    /* Find a free slot first */
    for (int i = 0; i < FRAG_MAX_SLOTS; i++) {
        if (!g_frags[i].active) {
            g_frags[i].active = true;
            g_frags[i].src    = src;
            g_frags[i].dst    = dst;
            g_frags[i].proto  = proto;
            g_frags[i].ip_id  = id;
            g_frags[i].buf    = kzalloc(FRAG_MAX_PAYLOAD);
            g_frags[i].filled = 0;
            g_frags[i].total_len = 0;
            g_frags[i].have_last = false;
            g_frags[i].age    = g_frag_age++;
            return g_frags[i].buf ? &g_frags[i] : NULL;
        }
    }

    /* No free slot — evict the oldest one (LRU) */
    frag_slot_t *oldest = &g_frags[0];
    for (int i = 1; i < FRAG_MAX_SLOTS; i++)
        if (g_frags[i].age < oldest->age) oldest = &g_frags[i];

    serial_print("[IP4] frag: evicting stale reassembly slot\n");
    if (oldest->buf) { kfree(oldest->buf); oldest->buf = NULL; }

    oldest->active   = true;
    oldest->src      = src;
    oldest->dst      = dst;
    oldest->proto    = proto;
    oldest->ip_id    = id;
    oldest->buf      = kzalloc(FRAG_MAX_PAYLOAD);
    oldest->filled   = 0;
    oldest->total_len = 0;
    oldest->have_last = false;
    oldest->age      = g_frag_age++;
    return oldest->buf ? oldest : NULL;
}

static void frag_free(frag_slot_t *s)
{
    if (s->buf) { kfree(s->buf); s->buf = NULL; }
    s->active = false;
}

/*
 * ip4_frag_input — handle a fragment.
 * Returns true if the datagram is now complete; fills *out_buf / *out_len.
 * Caller must kfree(*out_buf) after use.
 */
static bool ip4_frag_input(ip4_t src, ip4_t dst, uint8_t proto,
                            uint16_t ip_id, uint16_t frag_flags,
                            const uint8_t *payload, uint16_t payload_len,
                            uint8_t **out_buf, uint16_t *out_len)
{
    bool     more_frags   = (frag_flags & IP_FLAG_MF) != 0;
    uint16_t frag_offset  = (frag_flags & IP_FRAG_MASK) * 8;  /* bytes */

    /* Sanity: fragment would overflow the maximum IP payload */
    if ((uint32_t)frag_offset + payload_len > FRAG_MAX_PAYLOAD) {
        serial_print("[IP4] frag: offset overflow — dropping\n");
        return false;
    }

    frag_slot_t *slot = frag_find(src, dst, proto, ip_id);
    if (!slot) {
        slot = frag_alloc(src, dst, proto, ip_id);
        if (!slot) {
            serial_print("[IP4] frag: alloc failed — dropping\n");
            return false;
        }
    }
    slot->age = g_frag_age++;  /* touch to refresh LRU */

    /* Copy this fragment's payload into the right offset */
    memcpy(slot->buf + frag_offset, payload, payload_len);
    slot->filled += payload_len;

    if (!more_frags) {
        /* This is the last fragment — now we know the total size */
        slot->total_len  = frag_offset + payload_len;
        slot->have_last  = true;
    }

    /* Reassembly complete? (Simple check: received enough bytes) */
    if (slot->have_last && slot->filled >= slot->total_len) {
        /* Hand ownership to caller */
        *out_buf = slot->buf;
        *out_len = slot->total_len;
        slot->buf = NULL;   /* don't double-free in frag_free */
        frag_free(slot);
        return true;
    }

    return false;  /* still waiting for more fragments */
}

void ip4_init(void)
{
    g_ip_id = 0;
    for (int i = 0; i < FRAG_MAX_SLOTS; i++) {
        g_frags[i].active = false;
        g_frags[i].buf    = NULL;
    }
}

void ip4_input(netif_t *iface, netbuf_t *buf)
{
    if (buf->len < IP4_HDR_LEN) return;

    ip4_hdr_t *hdr = (ip4_hdr_t *)buf->data;

    /* Version check */
    if ((hdr->version_ihl >> 4) != 4) return;

    uint8_t  ihl     = (hdr->version_ihl & 0x0F) * 4;
    uint16_t tot_len = ntohs(hdr->total_len);

    if (ihl < IP4_HDR_LEN || tot_len > buf->len) return;

    /* Skip checksum verification — QEMU uses hardware checksum offloading */

    ip4_t src = ntohl(hdr->src);
    ip4_t dst = ntohl(hdr->dst);

    /* Accept packets for our IP, broadcast, or loopback */
    if (dst != iface->ip && dst != IP4_BROADCAST && dst != IP4(127,0,0,1)) {
        /* dst mismatch — silently drop */
        return;
    }

    /* CNSL: drop packets from blocked IPs before any processing */
    if (cnsl_is_blocked(src)) {
        serial_print("[CNSL] dropped packet from blocked IP\n");
        return;
    }

    uint16_t flags_offset = ntohs(hdr->flags_offset);
    bool     is_fragment  = (flags_offset & IP_FLAG_MF) ||
                            (flags_offset & IP_FRAG_MASK);

    /* Consume IP header — payload starts here */
    netbuf_pull(buf, ihl);
    buf->len = (uint16_t)(tot_len - ihl);

    if (is_fragment) {
        /*
         * Fragmented datagram — feed into reassembler.
         * ip4_frag_input returns true only when the datagram is complete.
         * On completion it kzalloc's a contiguous buffer; we wrap it in
         * a temporary netbuf, dispatch, then free the buffer.
         */
        uint8_t *reassembled = NULL;
        uint16_t full_len    = 0;

        bool complete = ip4_frag_input(src, dst, hdr->proto,
                                       ntohs(hdr->id), flags_offset,
                                       buf->data, buf->len,
                                       &reassembled, &full_len);
        if (!complete) return;   /* waiting for more fragments */

        /* Build a temporary netbuf from the reassembled payload */
        netbuf_t *rbuf = netbuf_alloc();
        if (!rbuf) { kfree(reassembled); return; }

        /* Copy reassembled payload into netbuf (capped at netbuf capacity) */
        uint16_t copy_len = full_len;
        if (copy_len > (uint16_t)(NETBUF_CAP - NETBUF_HEADROOM))
            copy_len = (uint16_t)(NETBUF_CAP - NETBUF_HEADROOM);
        memcpy(rbuf->data, reassembled, copy_len);
        rbuf->len = copy_len;
        kfree(reassembled);

        switch (hdr->proto) {
            case IP_PROTO_ICMP: icmp_input(iface, rbuf, src, dst); break;
            case IP_PROTO_TCP:  tcp_input(iface,  rbuf, src, dst); break;
            case IP_PROTO_UDP:  udp_input(iface,  rbuf, src, dst); break;
            default: break;
        }
        netbuf_free(rbuf);
        return;
    }

    /* Non-fragmented — dispatch directly */
    switch (hdr->proto) {
        case IP_PROTO_ICMP: icmp_input(iface, buf, src, dst); break;
        case IP_PROTO_TCP:  tcp_input(iface,  buf, src, dst); break;
        case IP_PROTO_UDP:  udp_input(iface,  buf, src, dst); break;
        default: break;
    }
}

bool ip4_output(netif_t *iface, ip4_t dst_ip,
                uint8_t proto, netbuf_t *buf)
{
    /* Prepend IP header */
    ip4_hdr_t *hdr = (ip4_hdr_t *)netbuf_push(buf, IP4_HDR_LEN);
    if (!hdr) return false;

    uint16_t tot_len = buf->len;

    hdr->version_ihl  = (4 << 4) | 5;
    hdr->dscp_ecn     = 0;
    hdr->total_len    = htons(tot_len);
    hdr->id           = htons(g_ip_id++);
    hdr->flags_offset = htons(0x4000);   /* Don't Fragment */
    hdr->ttl          = 64;
    hdr->proto        = proto;
    hdr->checksum     = 0;
    hdr->src          = htonl(iface->ip);
    hdr->dst          = htonl(dst_ip);
    hdr->checksum     = htons(inet_cksum(hdr, IP4_HDR_LEN));

    /*
     * Loopback: deliver straight to our own input path instead of
     * transmitting over the wire. Without this, a packet addressed
     * to 127.0.0.1 fell through to the normal ARP+Ethernet+NIC-
     * transmit path below — but nothing on the network segment
     * answers ARP for 127.0.0.1, so it was just dropped/never
     * delivered. That silently broke ALL localhost communication
     * (any socket connecting to 127.0.0.1, which is exactly the
     * normal way to test the local stack without going over the
     * network) even though ip4_input() already had loopback-address
     * accept logic on the *receiving* side — packets just never
     * actually got there.
     */
    if (dst_ip == IP4(127, 0, 0, 1)) {
        ip4_input(iface, buf);
        return true;
    }

    /* Resolve next-hop MAC via ARP */
    ip4_t next_hop = dst_ip;
    /* If outside subnet, route via gateway */
    if ((dst_ip & iface->mask) != (iface->ip & iface->mask))
        next_hop = iface->gw;

    mac_addr_t dst_mac;
    if (!arp_resolve(iface, next_hop, &dst_mac)) {
        /* ARP request sent — packet dropped, caller retries */
        return false;
    }

    /* Prepend Ethernet header */
    eth_hdr_t *eth = (eth_hdr_t *)netbuf_push(buf, sizeof(eth_hdr_t));
    if (!eth) return false;
    eth->dst  = dst_mac;
    eth->src  = iface->mac;
    eth->type = htons(ETHERTYPE_IP4);

    return iface->tx(iface, buf);
}