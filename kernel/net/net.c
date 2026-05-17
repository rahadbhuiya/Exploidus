#include "net.h"
#include "../mm/kmalloc.h"
#include "../drivers/serial.h"
#include <string.h>


/* Packet buffer pool */


#define NETBUF_POOL_SIZE 64

static netbuf_t g_pool[NETBUF_POOL_SIZE];
static bool g_pool_used[NETBUF_POOL_SIZE];

netbuf_t *netbuf_alloc(void)
{
    for (int i = 0; i < NETBUF_POOL_SIZE; i++) {
        if (!g_pool_used[i]) {
            g_pool_used[i] = true;

            netbuf_t *b = &g_pool[i];
            memset(b, 0, sizeof(netbuf_t));

            b->data = b->_storage + NETBUF_HEADROOM;
            b->len = 0;
            b->next = NULL;

            return b;
        }
    }

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

    g_pool_used[idx] = false;
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