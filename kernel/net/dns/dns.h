#pragma once
#include "../net.h"

/*
 * dns_resolve — synchronous A-record lookup using QEMU's DNS server
 * (10.0.2.3, standard QEMU user-networking DNS).
 * hostname : null-terminated domain name (e.g. "example.com")
 * Returns  : IPv4 address in host byte order, or 0 on failure/timeout.
 */
ip4_t dns_resolve(const char *hostname);