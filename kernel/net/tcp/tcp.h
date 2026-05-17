#pragma once
#include "../net.h"

/* TCP states */
typedef enum {
    TCP_CLOSED      = 0,
    TCP_LISTEN      = 1,
    TCP_SYN_SENT    = 2,
    TCP_SYN_RECV    = 3,
    TCP_ESTABLISHED = 4,
    TCP_FIN_WAIT1   = 5,
    TCP_FIN_WAIT2   = 6,
    TCP_CLOSE_WAIT  = 7,
    TCP_CLOSING     = 8,
    TCP_LAST_ACK    = 9,
    TCP_TIME_WAIT   = 10,
} tcp_state_t;

/* TCP header flags */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset;   /* top 4 bits = header length in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

#define TCP_HDR_LEN  20

/* Receive/send buffer per connection */
#define TCP_BUF_SIZE  4096

typedef struct tcp_conn {
    tcp_state_t state;
    ip4_t       local_ip;
    ip4_t       remote_ip;
    uint16_t    local_port;
    uint16_t    remote_port;

    uint32_t    snd_nxt;    /* next sequence number to send */
    uint32_t    snd_una;    /* oldest unacknowledged sequence number */
    uint32_t    rcv_nxt;    /* next expected receive sequence number */
    uint16_t    rcv_wnd;    /* our receive window */
    uint16_t    snd_wnd;    /* peer's advertised window */

    uint8_t     rx_buf[TCP_BUF_SIZE];
    uint16_t    rx_head;
    uint16_t    rx_tail;

    uint8_t     tx_buf[TCP_BUF_SIZE];
    uint16_t    tx_head;
    uint16_t    tx_tail;

    struct tcp_conn *next;
} tcp_conn_t;

void      tcp_init(void);
void      tcp_input(netif_t *iface, netbuf_t *buf, ip4_t src, ip4_t dst);
tcp_conn_t *tcp_connect(netif_t *iface, ip4_t dst_ip, uint16_t dst_port,
                         uint16_t src_port);
tcp_conn_t *tcp_listen(uint16_t port);
tcp_conn_t *tcp_accept(tcp_conn_t *listener);
int16_t   tcp_send(tcp_conn_t *conn, const void *data, uint16_t len);
int16_t   tcp_recv(tcp_conn_t *conn, void *buf, uint16_t len);
void      tcp_close(tcp_conn_t *conn);
bool      tcp_is_connected(tcp_conn_t *conn);
