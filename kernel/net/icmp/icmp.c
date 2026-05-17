#include "icmp.h"
#include "../ip/ip.h"
#include "../net.h"
#include "../../drivers/serial.h"
#include <string.h>

#define ICMP_ECHO_REQUEST  8
#define ICMP_ECHO_REPLY    0

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr_t;

/* Shared ping reply state */
static volatile uint16_t g_ping_reply_seq = 0xFFFF;
static volatile ip4_t    g_ping_reply_src = 0;

void icmp_ping_notify(ip4_t src, uint16_t seq)
{
    g_ping_reply_src = src;
    g_ping_reply_seq = seq;
}

void icmp_input(netif_t *iface, netbuf_t *buf, ip4_t src, ip4_t dst)
{
    (void)dst;
    if (buf->len < sizeof(icmp_hdr_t)) return;

    icmp_hdr_t *hdr = (icmp_hdr_t *)buf->data;

    /* Handle echo reply — notify waiting ping */
    if (hdr->type == ICMP_ECHO_REPLY) {
        serial_print("[ICMP] Echo reply from ");
        serial_printhex((uint64_t)src);
        serial_print(" seq=");
        serial_printhex((uint64_t)ntohs(hdr->seq));
        serial_print("\n");
        icmp_ping_notify(src, ntohs(hdr->seq));
        return;
    }

    if (hdr->type != ICMP_ECHO_REQUEST) return;

    /* Verify checksum */
    uint16_t saved = hdr->checksum;
    hdr->checksum  = 0;
    uint16_t cksum = inet_cksum(buf->data, buf->len);
    hdr->checksum  = saved;
    if (cksum != saved) {
        serial_print("[ICMP] bad checksum\n");
        return;
    }

    serial_print("[ICMP] Echo request from ");
    serial_printhex((uint64_t)src);
    serial_print(" seq=");
    serial_printhex((uint64_t)ntohs(hdr->seq));
    serial_print("\n");

    /* Build reply */
    netbuf_t *reply = netbuf_alloc();
    if (!reply) return;

    if (buf->len > NETBUF_CAP - NETBUF_HEADROOM - IP4_HDR_LEN - (int)sizeof(eth_hdr_t))
        goto done;

    memcpy(reply->data, buf->data, buf->len);
    reply->len = buf->len;

    icmp_hdr_t *r = (icmp_hdr_t *)reply->data;
    r->type     = ICMP_ECHO_REPLY;
    r->checksum = 0;
    r->checksum = inet_cksum(reply->data, reply->len);

    ip4_output(iface, src, IP_PROTO_ICMP, reply);
done:
    netbuf_free(reply);
}


/*  icmp_send_echo   */


int icmp_send_echo(ip4_t dst, uint16_t seq)
{
    netif_t *iface = netif_default();
    if (!iface || !iface->up) return -1;

    netbuf_t *buf = netbuf_alloc();
    if (!buf) return -1;

    icmp_hdr_t *hdr = (icmp_hdr_t *)buf->data;
    hdr->type     = ICMP_ECHO_REQUEST;
    hdr->code     = 0;
    hdr->checksum = 0;
    hdr->id       = htons(0x1337);
    hdr->seq      = htons(seq);

    uint8_t *payload = buf->data + sizeof(icmp_hdr_t);
    for (int i = 0; i < 32; i++)
        payload[i] = (uint8_t)i;

    buf->len = (uint16_t)(sizeof(icmp_hdr_t) + 32);
    hdr->checksum = inet_cksum(buf->data, buf->len);

    g_ping_reply_seq = 0xFFFF;
    g_ping_reply_src = 0;

    serial_print("[ICMP] Sending echo to ");
    serial_printhex((uint64_t)dst);
    serial_print("\n");

    ip4_output(iface, dst, IP_PROTO_ICMP, buf);
    netbuf_free(buf);

    /* Wait ~500ms for reply */
    extern void net_poll(void);
    extern void net_poll(void);
    for (uint32_t i = 0; i < 5000000; i++) {
        if (i % 10000 == 0) net_poll();
        if (i % 10000 == 0) net_poll();
        if (g_ping_reply_seq == seq)
            return 0;
        __asm__ volatile ("pause");
    }

    return -1;
}