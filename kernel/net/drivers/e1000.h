#pragma once
#include "../net.h"
#include <stdbool.h>

/*
 * Intel e1000 NIC driver.
 * Supports: QEMU -net nic,model=e1000
 *
 * The e1000 is memory-mapped. We locate it via PCI config space,
 * then map its BAR0 MMIO region and set up TX/RX descriptor rings.
 */

bool e1000_init(void);
bool e1000_transmit(netif_t *iface, netbuf_t *buf);
void e1000_poll(void);      /* called from network tick / IRQ */
