#include "socket.h"
#include "../net.h"
#include "../tcp/tcp.h"
#include "../udp/udp.h"
#include "../../drivers/serial.h"
#include "../../proc/scheduler.h"
#include "../../proc/process.h"
#include <string.h>

extern void net_poll(void);

static socket_t  g_sockets[MAX_SOCKETS];
static uint16_t  g_next_ephemeral = 49152;

void net_socket_init(void)
{
    memset(g_sockets, 0, sizeof(g_sockets));
}

static socket_t *sock_get(int fd)
{
    if (fd < 0 || fd >= MAX_SOCKETS) return NULL;
    if (!g_sockets[fd].active) return NULL;
    return &g_sockets[fd];
}

int net_socket(sock_type_t type)
{
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!g_sockets[i].active) {
            memset(&g_sockets[i], 0, sizeof(socket_t));
            g_sockets[i].type   = type;
            g_sockets[i].active = true;
            return i;
        }
    }
    return -1;
}

int net_bind(int fd, uint16_t port)
{
    socket_t *s = sock_get(fd);
    if (!s) return -1;
    s->local_port = port;
    return 0;
}

int net_connect(int fd, ip4_t ip, uint16_t port)
{
    socket_t *s = sock_get(fd);
    if (!s) return -1;

    netif_t *iface = netif_default();
    if (!iface) return -1;

    if (!s->local_port)
        s->local_port = g_next_ephemeral++;

    if (s->type == SOCK_TCP) {
        /* Loopback: if connecting to our own IP, bypass network */
        if (ip == iface->ip || ip == IP4(127,0,0,1)) {
            tcp_conn_t *listener = tcp_listen_find(port);
            if (!listener) return -1;

            /* Create connected pair directly */
            tcp_conn_t *server_side = tcp_conn_alloc_for_loopback(listener, s->local_port, iface->ip);
            tcp_conn_t *client_side = tcp_conn_alloc_for_loopback_client(port, s->local_port, iface->ip);

            if (!server_side || !client_side) return -1;

            /* Link partners so send goes to partner's rx_buf */
            server_side->partner = client_side;
            client_side->partner = server_side;

            s->tcp_conn    = client_side;
            s->remote_ip   = ip;
            s->remote_port = port;
            return 0;
        }

        s->tcp_conn = tcp_connect(iface, ip, port, s->local_port);
        if (!s->tcp_conn) return -1;
        s->remote_ip   = ip;
        s->remote_port = port;

        /* Spin until connected or timeout */
        uint32_t timeout = 50000;
        while (!tcp_is_connected(s->tcp_conn) && timeout--) {
            net_poll();
            sched_yield();
        }

        return tcp_is_connected(s->tcp_conn) ? 0 : -1;
    }

    if (s->type == SOCK_UDP) {
        s->remote_ip   = ip;
        s->remote_port = port;
        return 0;
    }

    return -1;
}

int net_listen(int fd)
{
    socket_t *s = sock_get(fd);
    if (!s || s->type != SOCK_TCP) return -1;

    s->tcp_conn = tcp_listen(s->local_port);
    return s->tcp_conn ? 0 : -1;
}

int net_accept(int fd)
{
    socket_t *s = sock_get(fd);
    if (!s || s->type != SOCK_TCP || !s->tcp_conn) return -1;

    /* Wait for an incoming connection */
    tcp_conn_t *accepted = NULL;
    for (;;) {
        net_poll();
        accepted = tcp_accept(s->tcp_conn);
        if (accepted) {
            serial_print("[SOCK] accept OK\n");
            break;
        }
        sched_yield();
    }

    /* Create a new socket for the accepted connection */
    int nfd = net_socket(SOCK_TCP);
    if (nfd < 0) { tcp_close(accepted); return -1; }

    g_sockets[nfd].tcp_conn    = accepted;
    g_sockets[nfd].local_port  = s->local_port;
    g_sockets[nfd].remote_ip   = accepted->remote_ip;
    g_sockets[nfd].remote_port = accepted->remote_port;
    return nfd;
}

int net_send(int fd, const void *buf, uint16_t len)
{
    socket_t *s = sock_get(fd);
    if (!s) return -1;

    netif_t *iface = netif_default();
    if (!iface) return -1;

    if (s->type == SOCK_TCP)
        return (int)tcp_send(s->tcp_conn, buf, len);

    if (s->type == SOCK_UDP)
        return udp_send(iface, s->remote_ip, s->local_port,
                        s->remote_port, buf, len) ? (int)len : -1;

    return -1;
}

int net_recv(int fd, void *buf, uint16_t len)
{
    socket_t *s = sock_get(fd);
    if (!s) return -1;

    if (s->type == SOCK_TCP) {
        if (!s->tcp_conn) return -1;

        /* Wait up to ~5 seconds for data */
        uint32_t timeout = 5000;
        int16_t n = 0;
        while (n == 0 && timeout > 0) {
            net_poll();
            n = tcp_recv(s->tcp_conn, buf, len);
            if (n > 0) { break; }
            if (s->tcp_conn->state == TCP_CLOSED ||
                s->tcp_conn->state == TCP_CLOSE_WAIT) break;
            sched_yield();
            timeout--;
        }
        return (int)n;
    }

    if (s->type == SOCK_UDP) {
        uint16_t avail = (uint16_t)((s->udp_rx_head - s->udp_rx_tail)
                                   & 4095);
        if (!avail) return 0;
        uint16_t n = avail < len ? avail : len;
        uint8_t *p = (uint8_t *)buf;
        for (uint16_t i = 0; i < n; i++) {
            p[i] = s->udp_rx[s->udp_rx_tail & 4095];
            s->udp_rx_tail++;
        }
        return (int)n;
    }

    return -1;
}

void net_socket_close(int fd)
{
    socket_t *s = sock_get(fd);
    if (!s) return;
    if (s->type == SOCK_TCP && s->tcp_conn)
        tcp_close(s->tcp_conn);
    s->active = false;
}