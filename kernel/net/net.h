#pragma once
#include <stdint.h>
#include <stdbool.h>


/*  Byte order helpers */


static inline uint16_t htons(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}
static inline uint16_t ntohs(uint16_t x) { return htons(x); }

static inline uint32_t htonl(uint32_t x)
{
    return ((x & 0xFF000000u) >> 24)
         | ((x & 0x00FF0000u) >>  8)
         | ((x & 0x0000FF00u) <<  8)
         | ((x & 0x000000FFu) << 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }


/*  MAC address    */


#define MAC_LEN 6

typedef struct { uint8_t b[MAC_LEN]; } mac_addr_t;

static inline bool mac_eq(mac_addr_t a, mac_addr_t b)
{
    for (int i = 0; i < MAC_LEN; i++)
        if (a.b[i] != b.b[i]) return false;
    return true;
}
static inline bool mac_is_broadcast(mac_addr_t m)
{
    for (int i = 0; i < MAC_LEN; i++)
        if (m.b[i] != 0xFF) return false;
    return true;
}


/*  IPv4 address — stored in HOST byte order    */


typedef uint32_t ip4_t;

#define IP4(a,b,c,d) \
    (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

#define IP4_BROADCAST  0xFFFFFFFFu
#define IP4_ANY        0x00000000u


/*  Packet buffer                                                        */
/*                                                                       */
/*  Each netbuf is a fixed 1536-byte storage block.                     */
/*  `data` points somewhere inside _storage.                            */
/*  `len`  is how many bytes are valid starting at `data`.              */
/*  Layers prepend headers by calling netbuf_push().                    */
/*  Layers consume headers by calling netbuf_pull().                    */

#define NETBUF_CAP      1536
#define NETBUF_HEADROOM  256   /* reserved at front for header prepends */

typedef struct netbuf {
    uint8_t        _storage[NETBUF_CAP];
    uint8_t       *data;
    uint16_t       len;
    struct netbuf *next;
} netbuf_t;

netbuf_t *netbuf_alloc(void);
void      netbuf_free(netbuf_t *buf);
uint8_t  *netbuf_push(netbuf_t *buf, uint16_t bytes);
uint8_t  *netbuf_pull(netbuf_t *buf, uint16_t bytes);


/*  Ethernet header   */


#define ETHERTYPE_IP4  0x0800
#define ETHERTYPE_ARP  0x0806

typedef struct __attribute__((packed)) {
    mac_addr_t dst;
    mac_addr_t src;
    uint16_t   type;   /* network byte order */
} eth_hdr_t;


/*  Network interface descriptor     */


typedef struct netif {
    char       name[8];
    mac_addr_t mac;
    ip4_t      ip;
    ip4_t      mask;
    ip4_t      gw;
    bool       up;

    bool (*tx)(struct netif *, netbuf_t *);

    netbuf_t  *rx_head;
    netbuf_t  *rx_tail;
    uint32_t   rx_dropped;
} netif_t;

#define MAX_NETIFS  4

void     netif_register(netif_t *iface);
netif_t *netif_default(void);
void     netif_rx_enqueue(netif_t *iface, netbuf_t *buf);
netbuf_t *netif_rx_dequeue(netif_t *iface);

/*  Internet checksum    */


uint16_t inet_cksum(const void *data, uint16_t len);
uint16_t inet_cksum_pseudo(ip4_t src, ip4_t dst,
                            uint8_t proto, uint16_t len,
                            const void *data, uint16_t data_len);
