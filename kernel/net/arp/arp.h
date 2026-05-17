#pragma once
#include "../net.h"

void arp_init(void);
void arp_input(netif_t *iface, netbuf_t *buf);
bool arp_resolve(netif_t *iface, ip4_t ip, mac_addr_t *out);
void arp_announce(netif_t *iface);
