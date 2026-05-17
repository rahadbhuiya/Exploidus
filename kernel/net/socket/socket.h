#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../net.h"
#include "../tcp/tcp.h"
#include "../udp/udp.h"

#define MAX_SOCKETS   32

typedef enum {
    SOCK_NONE  = 0,
    SOCK_TCP   = 1,
    SOCK_UDP   = 2,
} sock_type_t;

typedef struct {
    sock_type_t  type;
    bool         active;
    tcp_conn_t  *tcp_conn;   /* for TCP sockets */
    uint16_t     local_port; /* for UDP sockets */
    ip4_t        remote_ip;  /* for UDP sockets */
    uint16_t     remote_port;

    /* UDP receive ring */
    uint8_t      udp_rx[4096];
    uint16_t     udp_rx_head;
    uint16_t     udp_rx_tail;
} socket_t;

void  net_socket_init(void);
int   net_socket(sock_type_t type);
int   net_bind(int fd, uint16_t port);
int   net_connect(int fd, ip4_t ip, uint16_t port);
int   net_listen(int fd);
int   net_accept(int fd);
int   net_send(int fd, const void *buf, uint16_t len);
int   net_recv(int fd, void *buf, uint16_t len);
void  net_socket_close(int fd);
