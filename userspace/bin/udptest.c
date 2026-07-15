#include "../libc/syscall.h"

/*
 * udptest — verifies the UDP socket receive fix (socket_udp_recv_cb
 * bridge in kernel/net/socket/socket.c) actually works end-to-end.
 *
 * Before that fix, net_recv() on a SOCK_UDP socket could NEVER return
 * any data (udp_rx_head was written nowhere in the whole codebase) —
 * this test would have hung until its 5-second timeout and reported
 * "recv returned 0 (timeout / no data)" no matter what.
 *
 * Uses loopback (127.0.0.1) so it's fully self-contained: socket A
 * binds to a port and waits, socket B connects to A and sends a
 * message, A receives it and checks the content matches.
 */

static void print(const char *s) { puts(s); }
static void println(const char *s) { puts(s); putc('\n'); }

static void print_int(int64_t v)
{
    if (v < 0) { putc('-'); v = -v; }
    if (v == 0) { putc('0'); return; }
    char digits[20]; int n = 0;
    while (v > 0) { digits[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) putc(digits[--n]);
}

int main(void)
{
    cap_token_t null_cap = {0, 0};

    println("udptest: starting");

    int a = xsocket(null_cap, SOCK_UDP);
    if (a < 0) { println("[FAIL] create socket A"); return 1; }
    println("[ OK ] create socket A (UDP)");

    if (xbind(null_cap, a, 9999) < 0) {
        println("[FAIL] bind socket A to port 9999");
        return 1;
    }
    println("[ OK ] bind socket A to port 9999");

    int b = xsocket(null_cap, SOCK_UDP);
    if (b < 0) { println("[FAIL] create socket B"); return 1; }
    println("[ OK ] create socket B (UDP)");

    ip4_t loopback = IP4(127, 0, 0, 1);
    if (xconnect(null_cap, b, loopback, 9999) < 0) {
        println("[FAIL] connect socket B to 127.0.0.1:9999");
        return 1;
    }
    println("[ OK ] connect socket B to 127.0.0.1:9999");

    const char *msg = "hello udp";
    int sent = xsend(null_cap, b, msg, 9);
    if (sent != 9) { println("[FAIL] send from socket B"); return 1; }
    println("[ OK ] sent 9 bytes from socket B");

    println("udptest: waiting for socket A to receive (up to ~5s)...");

    char buf[32];
    int n = xrecv(null_cap, a, buf, sizeof(buf) - 1);
    if (n <= 0) {
        println("[FAIL] recv on socket A returned 0 (timeout / no data)");
        println("udptest: this is exactly the bug the fix addresses -- if you");
        println("see this FAIL, the socket_udp_recv_cb bridge isn't working.");
        return 1;
    }
    buf[n] = '\0';
    print("[ OK ] socket A received "); print_int(n); print(" bytes: \"");
    print(buf);
    println("\"");

    int match = (n == 9 &&
                 buf[0]=='h' && buf[1]=='e' && buf[2]=='l' && buf[3]=='l' &&
                 buf[4]=='o' && buf[5]==' ' && buf[6]=='u' && buf[7]=='d' &&
                 buf[8]=='p');
    if (match) println("[ OK ] content matches what was sent");
    else       println("[FAIL] content does NOT match what was sent");

    println("udptest: done");
    return match ? 0 : 1;
}