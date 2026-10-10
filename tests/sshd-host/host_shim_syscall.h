#pragma once
/* HOST TEST SHIM: replaces Exploidus's userspace/libc/syscall.h so the
 * real sshd.c / sshd_packet.c / sshd_kex.c compile unmodified against
 * Linux sockets. Not part of the project. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef struct { uint64_t upper, lower; } cap_token_t;
#define SOCK_TCP 1
#define STDOUT_FILENO 1

static inline int xsocket(cap_token_t c, int t) {
    (void)c; (void)t;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    return fd;
}
static inline int xbind(cap_token_t c, int fd, uint16_t port) {
    (void)c; (void)port;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)atoi(getenv("SSHD_TEST_PORT")));
    return bind(fd, (struct sockaddr *)&a, sizeof a);
}
static inline int xlisten(cap_token_t c, int fd) { (void)c; return listen(fd, 4); }
static inline int xaccept(cap_token_t c, int fd) { (void)c; return accept(fd, 0, 0); }
static inline int xsend(cap_token_t c, int fd, const void *b, uint16_t n) { (void)c; return (int)send(fd, b, n, MSG_NOSIGNAL); }
static inline int xrecv(cap_token_t c, int fd, void *b, uint16_t n) { (void)c; return (int)recv(fd, b, n, 0); }
static inline void xclose(int fd) { close(fd); }
static inline int getrandom(void *b, uint64_t n) {
    int f = open("/dev/urandom", O_RDONLY); if (f < 0) return -1;
    ssize_t r = read(f, b, n); close(f); return r == (ssize_t)n ? 0 : -1;
}
static inline void puts(const char *s) { ssize_t r = write(1, s, strlen(s)); (void)r; }
static inline void putc(char c) { ssize_t r = write(1, &c, 1); (void)r; }
static inline int strcmp_u(const char *a, const char *b) { return strcmp(a, b); }
static inline int64_t sleep_ticks(uint64_t t) { usleep((useconds_t)(t * 1000)); return 0; }