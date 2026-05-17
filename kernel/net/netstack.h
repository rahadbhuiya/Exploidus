#pragma once
#include "net.h"

void eth_input(netif_t *iface, netbuf_t *buf);
void net_init(void);
void net_poll(void);
