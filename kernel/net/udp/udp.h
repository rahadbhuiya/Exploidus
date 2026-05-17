#pragma once
#include "../net.h"

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_hdr_t;

#define UDP_HDR_LEN  8

/* Callback type for UDP listeners */
typedef void (*udp_recv_fn)(netif_t *iface,
                             ip4_t src_ip, uint16_t src_port,
                             const uint8_t *data, uint16_t len);

void udp_init(void);
void udp_input(netif_t *iface, netbuf_t *buf, ip4_t src, ip4_t dst);
bool udp_send(netif_t *iface, ip4_t dst_ip, uint16_t src_port,
              uint16_t dst_port, const void *data, uint16_t len);
bool udp_bind(uint16_t port, udp_recv_fn cb);
