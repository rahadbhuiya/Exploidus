#pragma once
#include "../net.h"

void icmp_input(netif_t *iface, netbuf_t *buf, ip4_t src, ip4_t dst);

/*
 * icmp_send_echo — send one ICMP echo request to dst with given seq.
 * Blocks up to ~500 ms waiting for a reply.
 * Returns 0 on success, -1 on timeout.
 */
int  icmp_send_echo(ip4_t dst, uint16_t seq);

/*
 * icmp_ping_notify — called by icmp_input when an echo REPLY arrives.
 * Updates internal state so icmp_send_echo can detect the reply.
 */
void icmp_ping_notify(ip4_t src, uint16_t seq);