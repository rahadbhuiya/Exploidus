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

/*
 * socket_udp_recv_cb — bridges the callback-based UDP layer
 * (udp_bind(), used directly by e.g. DNS) to the socket_t-based API
 * (net_socket(SOCK_UDP) + net_recv()).
 *
 * This didn't exist before: net_bind() never actually registered
 * anything with the UDP layer, so udp_input() had no way to know a
 * socket wanted packets for a given port — g_sockets[i].udp_rx_head
 * was written nowhere in the whole codebase. Every UDP socket's
 * net_recv() could only ever return 0 (nothing received), forever.
 * Went unnoticed because DNS resolution bypasses sockets entirely via
 * its own direct udp_bind() call.
 */
static void socket_udp_recv_cb(netif_t *iface, ip4_t src_ip, uint16_t src_port,
                                uint16_t dst_port, const uint8_t *data, uint16_t len)
{
    (void)iface;

    for (int i = 0; i < MAX_SOCKETS; i++) {
        socket_t *s = &g_sockets[i];
        if (!s->active || s->type != SOCK_UDP) continue;
        if (s->local_port != dst_port) continue;

        /* If connect() was called, only accept datagrams from that
         * specific peer (matches connected-UDP-socket semantics). An
         * unconnected (just bound) socket accepts from anyone. */
        if (s->remote_port != 0 &&
            (s->remote_ip != src_ip || s->remote_port != src_port))
            continue;

        for (uint16_t k = 0; k < len; k++) {
            uint16_t next_head = (uint16_t)((s->udp_rx_head + 1) & 4095);
            if (next_head == (s->udp_rx_tail & 4095)) break; /* ring full — drop the rest, standard UDP behavior */
            s->udp_rx[s->udp_rx_head & 4095] = data[k];
            s->udp_rx_head = next_head;
        }
        return;
    }
    /* No socket bound to this port — silently drop, same as the
     * callback-layer's own "no listener" behavior. */
}

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

    if (s->type == SOCK_UDP) {
        if (!udp_bind(port, socket_udp_recv_cb)) return -1;
    }

    return 0;
}

int net_connect(int fd, ip4_t ip, uint16_t port)
{
    socket_t *s = sock_get(fd);
    if (!s) return -1;

    netif_t *iface = netif_default();
    if (!iface) return -1;

    if (!s->local_port) {
        s->local_port = g_next_ephemeral++;
        if (s->type == SOCK_UDP) udp_bind(s->local_port, socket_udp_recv_cb);
    }

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

        /* Spin until connected or timeout. Real wall-clock deadline
         * (g_uptime_ticks, 100Hz PIT) rather than a loop-iteration
         * count — an iteration count doesn't reliably measure elapsed
         * time since how long each iteration takes depends on system
         * load (how much work net_poll()/sched_yield() end up doing),
         * so the old count-based version could time out far sooner or
         * later than intended depending on what else was running. */
        uint64_t deadline = g_uptime_ticks + 500; /* ~5 seconds */
        while (!tcp_is_connected(s->tcp_conn) && g_uptime_ticks < deadline) {
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

        /* Wait up to ~5 seconds for data — real wall-clock deadline,
         * same reasoning as net_connect()'s fix above. */
        uint64_t deadline = g_uptime_ticks + 500;
        int16_t n = 0;
        while (n == 0 && g_uptime_ticks < deadline) {
            net_poll();
            n = tcp_recv(s->tcp_conn, buf, len);
            if (n > 0) break;

            /* Connection closing — but the remote may have sent data
             * right before the FIN (HTTP/1.0 Connection: close does
             * exactly this: full body followed immediately by FIN).
             * If tcp_recv returned 0 here, it means the receive buffer
             * is genuinely empty NOW.  Give it one more net_poll pass
             * to ensure any still-queued DMA frames are flushed into
             * the TCP buffer before concluding there is nothing left. */
            if (s->tcp_conn->state == TCP_CLOSED ||
                s->tcp_conn->state == TCP_CLOSE_WAIT) {
                net_poll();
                n = tcp_recv(s->tcp_conn, buf, len);
                break;  /* whatever we got, we're done */
            }

            sched_yield();
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