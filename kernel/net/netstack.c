#include "netstack.h"
#include "net.h"
#include "arp/arp.h"
#include "ip/ip.h"
#include "tcp/tcp.h"
#include "udp/udp.h"
#include "socket/socket.h"
#include "drivers/e1000.h"
#include "../drivers/serial.h"
#include <string.h>


/*  Ethernet frame dispatcher   */


void eth_input(netif_t *iface, netbuf_t *buf)
{
    if (buf->len < (uint16_t)sizeof(eth_hdr_t)) {
        netbuf_free(buf);
        return;
    }

    eth_hdr_t *eth = (eth_hdr_t *)netbuf_pull(buf, sizeof(eth_hdr_t));
    if (!eth) { netbuf_free(buf); return; }

    uint16_t type = ntohs(eth->type);
    /* Drop IPv6 */
    if (type == 0x86DD) { netbuf_free(buf); return; }

    switch (type) {
        case ETHERTYPE_ARP:
            arp_input(iface, buf);
            break;
        case ETHERTYPE_IP4:
            ip4_input(iface, buf);
            break;
        default:
            /* Unknown ethertype — drop silently */
            break;
    }

    netbuf_free(buf);
}


/*  Full network stack initialization     */


void net_init(void)
{
    serial_print("[NET ] Initializing network stack...\n");

    /* Core packet buffer pool and interface registry */
    /* (these are static arrays — no explicit init needed beyond zeroing
     *  which the BSS guarantees. But we call the subsystem inits
     *  to be explicit.) */

    arp_init();
    serial_print("[NET ] ARP cache initialized\n");

    ip4_init();
    serial_print("[NET ] IPv4 layer ready\n");

    tcp_init();
    serial_print("[NET ] TCP state machine ready\n");

    udp_init();
    serial_print("[NET ] UDP layer ready\n");

    net_socket_init();
    serial_print("[NET ] Socket layer ready\n");

    /* Probe for e1000 NIC */
    if (e1000_init()) {
        netif_t *iface = netif_default();
        if (iface) {
            arp_announce(iface);
            serial_print("[NET ] Gratuitous ARP sent\n");
            /* Poll a few times to receive any immediate ARP replies */
            for (int i = 0; i < 100; i++) e1000_poll();
            /* Pre-resolve gateway */
            mac_addr_t gw_mac;
            arp_resolve(iface, iface->gw, &gw_mac);
            for (int i = 0; i < 10000; i++) e1000_poll();
        }
    } else {
        serial_print("[NET ] No NIC found — network unavailable\n");
    }

    serial_print("[NET ] Stack online\n");
}


/*  net_poll — drain the NIC receive ring       */
/*                                                                       */
/*  Called from the idle loop and/or from a timer IRQ.                  */
/*  The e1000 does not use IRQ-driven receive in this implementation;   */
/*  instead we poll on every scheduler tick.                            */

void net_poll(void)
{
    /*
     * TEMPORARY: e1000_poll() is getting stuck in a cycle that never
     * returns (observed via QEMU monitor: RIP parked forever at the
     * E1000_RDH MMIO read right at the top of e1000_poll(), across
     * multiple samples seconds apart, with interrupts still disabled
     * -- meaning every timer tick calling this from sched_tick()
     * never comes back, so no further tick can ever fire and the
     * whole system freezes). Disabled here so fork()/kill()/signal
     * work (which needs a live timer tick to schedule/wake sleeping
     * processes) can be tested and verified independently of this.
     * This is NOT a fix -- the actual e1000 bug still needs to be
     * found and fixed, then this call restored.
     */
    (void)0;
}