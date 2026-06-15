/*
 * httptest.c — Simple HTTP client to test httpd from inside Exploidus
 *
 * Connects to localhost:80 and sends a GET request.
 * Tests the full network stack without QEMU NAT issues.
 */

#include "../libc/syscall.h"

static void print(const char *s) { puts(s); }
static void println(const char *s) { puts(s); putc('\n'); }

static void print_int(int64_t n) {
    if (n < 0) { putc('-'); n = -n; }
    if (n == 0) { putc('0'); return; }
    char buf[21]; int i = 0;
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) putc(buf[i]);
}

int main(void)
{
    println("=== Exploidus HTTP Test Client ===");

    /* Get path from argv - for now hardcode check via uptime trick */
    /* Since we don't have argc/argv, use a simple approach */
    const char *path = "/huddlecluster";  /* change as needed */

    println("Connecting to 10.0.2.15:80...");

    cap_token_t null_cap = {0, 0};

    int fd = xsocket(null_cap, SOCK_TCP);
    if (fd < 0) { println("Error: socket failed"); return 1; }

    ip4_t ip = IP4(10, 0, 2, 15);
    if (xconnect(null_cap, fd, ip, 80) < 0) {
        println("Error: connect failed"); return 1;
    }

    println("Connected! Sending GET request...");

    static char req[256];
    /* Build: GET <path> HTTP/1.0\r\nHost: localhost\r\n\r\n */
    int i = 0;
    const char *s = "GET ";
    while (*s) req[i++] = *s++;
    s = path;
    while (*s) req[i++] = *s++;
    s = " HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    while (*s) req[i++] = *s++;
    req[i] = 0;

    int sent = xsend(null_cap, fd, req, (uint16_t)i);
    print("Sent "); print_int(sent); println(" bytes");

    /* Read response */
    println("--- Response ---");
    static char buf[512];
    int total = 0;
    int n;

    while ((n = xrecv(null_cap, fd, buf, 511)) > 0) {
        buf[n] = 0;
        puts(buf);
        total += n;
        if (total > 2000) break;
    }

    println("\n--- End ---");
    print("Total received: ");
    print_int(total);
    println(" bytes");

    xclose(fd);
    return 0;
}