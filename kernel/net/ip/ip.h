#pragma once
#include "../net.h"

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

typedef struct __attribute__((packed)) {
    uint8_t  version_ihl;   /* version=4, ihl=5 (20 bytes, no options) */
    uint8_t  dscp_ecn;
    uint16_t total_len;     /* total length including header */
    uint16_t id;
    uint16_t flags_offset;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;           /* network byte order */
    uint32_t dst;           /* network byte order */
} ip4_hdr_t;

#define IP4_HDR_LEN  20

void ip4_init(void);
void ip4_input(netif_t *iface, netbuf_t *buf);
bool ip4_output(netif_t *iface, ip4_t dst,
                uint8_t proto, netbuf_t *buf);
