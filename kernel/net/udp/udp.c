#include "udp.h"
#include "../ip/ip.h"
#include "../net.h"
#include "../../drivers/serial.h"
#include <string.h>

#define UDP_BINDINGS_MAX  16

typedef struct {
    uint16_t    port;
    udp_recv_fn cb;
    bool        active;
} udp_binding_t;

static udp_binding_t g_bindings[UDP_BINDINGS_MAX];

void udp_init(void)
{
    memset(g_bindings, 0, sizeof(g_bindings));
}

bool udp_bind(uint16_t port, udp_recv_fn cb)
{
    for (int i = 0; i < UDP_BINDINGS_MAX; i++) {
        if (!g_bindings[i].active) {
            g_bindings[i].port   = port;
            g_bindings[i].cb     = cb;
            g_bindings[i].active = true;
            return true;
        }
    }
    return false;
}

void udp_input(netif_t *iface, netbuf_t *buf, ip4_t src, ip4_t dst)
{
    (void)dst;
    if (buf->len < UDP_HDR_LEN) return;

    udp_hdr_t *hdr     = (udp_hdr_t *)buf->data;
    uint16_t   dst_port = ntohs(hdr->dst_port);
    uint16_t   src_port = ntohs(hdr->src_port);
    uint16_t   udp_len  = ntohs(hdr->length);

    if (udp_len < UDP_HDR_LEN || udp_len > buf->len) return;

    const uint8_t *payload     = buf->data + UDP_HDR_LEN;
    uint16_t       payload_len = (uint16_t)(udp_len - UDP_HDR_LEN);

    /* Dispatch to registered listener */
    for (int i = 0; i < UDP_BINDINGS_MAX; i++) {
        if (g_bindings[i].active && g_bindings[i].port == dst_port) {
            g_bindings[i].cb(iface, src, src_port, payload, payload_len);
            return;
        }
    }

    /* No listener — silently drop */
    (void)src_port;
}

bool udp_send(netif_t *iface, ip4_t dst_ip,
              uint16_t src_port, uint16_t dst_port,
              const void *data, uint16_t len)
{
    netbuf_t *buf = netbuf_alloc();
    if (!buf) return false;

    /* Copy payload */
    uint16_t payload_len = len;
    if (payload_len > NETBUF_CAP - NETBUF_HEADROOM - IP4_HDR_LEN
                    - (uint16_t)sizeof(eth_hdr_t) - UDP_HDR_LEN)
        payload_len = NETBUF_CAP - NETBUF_HEADROOM - IP4_HDR_LEN
                    - (uint16_t)sizeof(eth_hdr_t) - UDP_HDR_LEN;

    memcpy(buf->data, data, payload_len);
    buf->len = payload_len;

    /* Prepend UDP header */
    udp_hdr_t *hdr = (udp_hdr_t *)netbuf_push(buf, UDP_HDR_LEN);
    if (!hdr) { netbuf_free(buf); return false; }

    uint16_t udp_len = (uint16_t)(UDP_HDR_LEN + payload_len);
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length   = htons(udp_len);
    hdr->checksum = 0;

    /* UDP checksum (optional for IPv4 but we compute it) */
    hdr->checksum = inet_cksum_pseudo(iface->ip, dst_ip,
                                      IP_PROTO_UDP, udp_len,
                                      buf->data, buf->len);

    bool ok = ip4_output(iface, dst_ip, IP_PROTO_UDP, buf);
    netbuf_free(buf);
    return ok;
}
