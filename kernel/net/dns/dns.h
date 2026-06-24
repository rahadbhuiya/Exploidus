#pragma once
#include "../net.h"

/*
 * dns_resolve — synchronous A-record lookup using QEMU's DNS server
 *               (10.0.2.3, standard QEMU user-networking DNS).
 *
 * hostname : null-terminated domain name (e.g. "example.com")
 * Returns  : IPv4 address in host byte order, or 0 on failure/timeout.
 *
 * Sends one UDP query to 10.0.2.3:53 and spins on net_poll() until a
 * response arrives or the retry limit is hit.  Not re-entrant: only
 * one outstanding DNS query at a time (sufficient for rahu's serial
 * install workflow).
 */
ip4_t dns_resolve(const char *hostname);