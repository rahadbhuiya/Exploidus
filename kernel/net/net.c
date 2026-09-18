#include "net.h"
#include "../mm/kmalloc.h"
#include "../drivers/serial.h"
#include "../sync/sync.h"
#include <string.h>


/* Packet buffer pool */


#define NETBUF_POOL_SIZE 64

static netbuf_t g_pool[NETBUF_POOL_SIZE];
static bool g_pool_used[NETBUF_POOL_SIZE];
static spinlock_t g_pool_lock;

netbuf_t *netbuf_alloc(void)
{
    /*
     * BUG FIX: net_poll() (kernel/net/netstack.c) runs on every 100Hz
     * timer tick, from inside the timer IRQ handler itself
     * (sched_tick() -> net_poll(), kernel/proc/scheduler.c) --
     * meaning it can fire, and call back into TCP code that itself
     * calls netbuf_alloc()/netbuf_free(), literally *while* some
     * other kernel code (a process's own tcp_send_segment() call, for
     * instance) is in the middle of using this same pool. g_pool_used[]
     * is shared mutable state with no protection against that: two
     * interleaved scans could both land on the same "free" slot before
     * either marks it used, handing out one physical netbuf_t to two
     * live callers at once -- exactly the kind of bug that stayed
     * invisible as long as only one process ever meaningfully got CPU
     * time at once (see the scheduler-starvation fix elsewhere in this
     * tree) and started producing General Protection Faults the moment
     * two daemons could truly run concurrently.
     *
     * First attempt at this fix used a bare cli/sti pair, which was
     * wrong: when netbuf_alloc() is reached from net_poll() while
     * *already inside* the timer IRQ handler, IF is already 0 (the
     * IDT interrupt gate cleared it on entry) -- an unconditional
     * "sti" there re-enables interrupts prematurely, mid-ISR, letting
     * another IRQ nest on top of it, which produced a hang instead of
     * a crash. spin_lock_irqsave()/spin_unlock_irqrestore()
     * (kernel/sync/sync.c) exist specifically for this: they save the
     * actual RFLAGS.IF on entry and only restore that same value
     * afterward, so calling it from IRQ context correctly leaves
     * interrupts off, and calling it from process context correctly
     * leaves them on.
     */
    uint64_t flags = spin_lock_irqsave(&g_pool_lock);
    for (int i = 0; i < NETBUF_POOL_SIZE; i++) {
        if (!g_pool_used[i]) {
            g_pool_used[i] = true;
            spin_unlock_irqrestore(&g_pool_lock, flags);

            netbuf_t *b = &g_pool[i];
            memset(b, 0, sizeof(netbuf_t));

            b->data = b->_storage + NETBUF_HEADROOM;
            b->len = 0;
            b->next = NULL;

            return b;
        }
    }
    spin_unlock_irqrestore(&g_pool_lock, flags);

    serial_print("[NET] pool exhausted\n");
    return NULL;
}

void netbuf_free(netbuf_t *buf)
{
    if (!buf)
        return;

    /* SAFE INDEX CHECK */
    uintptr_t base = (uintptr_t)g_pool;
    uintptr_t ptr  = (uintptr_t)buf;

    if (ptr < base || ptr >= base + sizeof(g_pool)) {
        serial_print("[NET] invalid free ignored\n");
        return;
    }

    int idx = (int)((ptr - base) / sizeof(netbuf_t));

    if (idx < 0 || idx >= NETBUF_POOL_SIZE)
        return;

    /* Same reasoning as netbuf_alloc() -- see its comment. */
    uint64_t flags = spin_lock_irqsave(&g_pool_lock);
    g_pool_used[idx] = false;
    spin_unlock_irqrestore(&g_pool_lock, flags);
}


/* Buffer operations   */


uint8_t *netbuf_push(netbuf_t *buf, uint16_t bytes)
{
    if (!buf)
        return NULL;

    if (bytes > (buf->data - buf->_storage)) {
        serial_print("[NET] push no headroom\n");
        return NULL;
    }

    buf->data -= bytes;
    buf->len  += bytes;

    return buf->data;
}

uint8_t *netbuf_pull(netbuf_t *buf, uint16_t bytes)
{
    if (!buf || bytes > buf->len)
        return NULL;

    uint8_t *old = buf->data;

    buf->data += bytes;
    buf->len  -= bytes;

    return old;
}


/* Interface registry  */


static netif_t *g_netifs[MAX_NETIFS];
static int g_netif_count = 0;

void netif_register(netif_t *iface)
{
    if (!iface)
        return;

    if (g_netif_count >= MAX_NETIFS) {
        serial_print("[NET] iface table full\n");
        return;
    }

    g_netifs[g_netif_count++] = iface;

    serial_print("[NET] iface registered\n");
    serial_print(iface->name);
    serial_print("\n");
}

netif_t *netif_default(void)
{
    for (int i = 0; i < g_netif_count; i++) {
        netif_t *iface = g_netifs[i];

        if (iface && iface->up)
            return iface;
    }

    return NULL;
}


/* RX queue    */

void netif_rx_enqueue(netif_t *iface, netbuf_t *buf)
{
    if (!iface || !buf)
        return;

    buf->next = NULL;

    if (!iface->rx_tail) {
        iface->rx_head = buf;
        iface->rx_tail = buf;
    } else {
        iface->rx_tail->next = buf;
        iface->rx_tail = buf;
    }
}

netbuf_t *netif_rx_dequeue(netif_t *iface)
{
    if (!iface)
        return NULL;

    netbuf_t *buf = iface->rx_head;

    if (!buf)
        return NULL;

    iface->rx_head = buf->next;

    if (!iface->rx_head)
        iface->rx_tail = NULL;

    buf->next = NULL;
    return buf;
}


/* Checksum  */


uint16_t inet_cksum(const void *data, uint16_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }

    if (len == 1)
        sum += (uint16_t)p[0] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}


/* Pseudo checksum   */


uint16_t inet_cksum_pseudo(ip4_t src, ip4_t dst,
                            uint8_t proto, uint16_t seg_len,
                            const void *data, uint16_t data_len)
{
    uint32_t sum = 0;

    sum += (src >> 16) & 0xFFFF;
    sum += (src      ) & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF;
    sum += (dst      ) & 0xFFFF;

    sum += proto;
    sum += seg_len;

    const uint8_t *p = (const uint8_t *)data;
    uint16_t n = data_len;

    while (n > 1) {
        sum += (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        n -= 2;
    }

    if (n == 1)
        sum += (uint16_t)p[0] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}