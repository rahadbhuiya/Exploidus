/*
 * sshd_packet.c -- SSH binary packet protocol (RFC 4253 Section 6)
 * and SSH_MSG_KEXINIT (Section 7.1) construction/parsing.
 *
 * This is protocol framing/bookkeeping, not cryptographic primitive
 * code -- written directly from RFC 4253's own field-by-field
 * description rather than ported from a reference, the same way
 * chacha20poly1305.c's AEAD glue was (see that file's comment):
 * there's no equivalent "donna"-style reference for packet framing,
 * and the risk profile is different from field arithmetic -- a
 * framing bug produces a packet that fails to parse (loud, easy to
 * notice) rather than a subtly wrong number (quiet, easy to miss).
 */

#include <stdint.h>
#include <stddef.h>
#include "../libc/syscall.h"
#include "sshd_packet.h"

#define SSH_MSG_KEXINIT 20

static void print(const char *s)   { puts(s); }
static void println(const char *s) { puts(s); putc('\n'); }

/*
 * Tiny local mem helpers instead of <string.h> -- including both
 * <string.h> and "../libc/syscall.h" directly conflicts (string.h
 * declares a real slen(), syscall.h separately defines its own
 * static inline slen() guarded only by __EXPLOIDUS_LIBC__, with no
 * _STRING_H_-style guard the way its puts()/putc() have against
 * _STDIO_H_ -- see sshd.c's own comment on the analogous puts/putc
 * conflict). Simplest fix is avoiding <string.h> here entirely.
 */
static void mcopy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}
static int mcompare(const void *a, const void *b, uint32_t n) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (uint32_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

/* ---- reliable send/recv over a stream socket ---- */

static int send_all(cap_token_t cap, int fd, const uint8_t *buf, uint32_t len) {
    uint32_t off = 0;
    while (off < len) {
        uint32_t chunk = (len - off) > 4096 ? 4096 : (len - off);
        int n = xsend(cap, fd, buf + off, (uint16_t)chunk);
        if (n <= 0) return 0;
        off += (uint32_t)n;
    }
    return 1;
}

static int recv_exact(cap_token_t cap, int fd, uint8_t *buf, uint32_t len) {
    uint32_t off = 0;
    while (off < len) {
        uint32_t chunk = (len - off) > 4096 ? 4096 : (len - off);
        int n = xrecv(cap, fd, buf + off, (uint16_t)chunk);
        if (n <= 0) return 0;
        off += (uint32_t)n;
    }
    return 1;
}

/* ---- big-endian uint32 helpers ---- */

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)(v);
}

static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* ---- binary packet protocol (RFC 4253 Section 6) ---- */

int ssh_packet_send(cap_token_t cap, int fd, const uint8_t *payload,
                     uint32_t payload_len) {
    static uint8_t buf[SSH_PACKET_MAX + 64];
    const uint32_t block_size = 8; /* unencrypted minimum, RFC 4253 Section 6 */

    if (payload_len > SSH_PACKET_MAX)
        return 0;

    uint32_t padded = 1 + payload_len; /* padding_length byte + payload */
    uint32_t pad = block_size - (padded % block_size);
    if (pad < 4) pad += block_size;

    uint32_t packet_length = 1 + payload_len + pad;
    uint32_t total = 4 + packet_length;
    if (total > sizeof(buf))
        return 0;

    put_u32(buf, packet_length);
    buf[4] = (uint8_t)pad;
    mcopy(buf + 5, payload, payload_len);

    uint8_t padbuf[256];
    uint32_t off = 0;
    while (off < pad) {
        uint32_t chunk = (pad - off) > sizeof(padbuf) ? sizeof(padbuf) : (pad - off);
        getrandom(padbuf, chunk);
        mcopy(buf + 5 + payload_len + off, padbuf, chunk);
        off += chunk;
    }

    return send_all(cap, fd, buf, total);
}

int32_t ssh_packet_recv(cap_token_t cap, int fd, uint8_t *out) {
    uint8_t lenbuf[4];
    if (!recv_exact(cap, fd, lenbuf, 4))
        return -1;

    uint32_t packet_length = get_u32(lenbuf);
    /* RFC 4253 Section 6.1 recommends rejecting absurd lengths before
     * ever trying to allocate/read them -- 5 is the smallest possible
     * (padding_length byte + 4 bytes minimum padding, zero payload),
     * SSH_PACKET_MAX bounds the other end against both malicious and
     * simply-too-large-for-this-implementation packets. */
    if (packet_length < 5 || packet_length > SSH_PACKET_MAX)
        return -1;

    static uint8_t buf[SSH_PACKET_MAX];
    if (!recv_exact(cap, fd, buf, packet_length))
        return -1;

    uint8_t padding_length = buf[0];
    if ((uint32_t)padding_length + 1 > packet_length)
        return -1;

    uint32_t payload_len = packet_length - padding_length - 1;
    mcopy(out, buf + 1, payload_len);
    return (int32_t)payload_len;
}

/* ---- SSH_MSG_KEXINIT (RFC 4253 Section 7.1) ---- */

/* This server's one and only supported algorithm per category --
 * see the crypto primitives in kernel/crypto/ this corresponds to.
 * No fallback/legacy algorithms offered: a real client that doesn't
 * support one of these will simply fail to negotiate, which is
 * correct (this implementation genuinely can't speak anything else
 * yet) rather than silently degrading to something weaker. */
#define KEX_ALGORITHMS      "curve25519-sha256"
#define HOST_KEY_ALGORITHMS "ssh-ed25519"
#define CIPHER_ALGORITHMS   "chacha20-poly1305@openssh.com"
#define MAC_ALGORITHMS      "" /* AEAD cipher, no separate MAC per RFC 8439 Section 4 / OpenSSH convention */
#define COMPRESSION_ALGORITHMS "none"
#define LANGUAGES           ""

static uint32_t put_namelist(uint8_t *out, const char *s) {
    uint32_t len = (uint32_t)slen(s);
    put_u32(out, len);
    mcopy(out + 4, s, len);
    return 4 + len;
}

uint32_t ssh_kexinit_build(uint8_t *out, const uint8_t cookie[16]) {
    uint32_t pos = 0;

    out[pos++] = SSH_MSG_KEXINIT;
    mcopy(out + pos, cookie, 16); pos += 16;

    pos += put_namelist(out + pos, KEX_ALGORITHMS);
    pos += put_namelist(out + pos, HOST_KEY_ALGORITHMS);
    pos += put_namelist(out + pos, CIPHER_ALGORITHMS);      /* enc c->s */
    pos += put_namelist(out + pos, CIPHER_ALGORITHMS);      /* enc s->c */
    pos += put_namelist(out + pos, MAC_ALGORITHMS);         /* mac c->s */
    pos += put_namelist(out + pos, MAC_ALGORITHMS);         /* mac s->c */
    pos += put_namelist(out + pos, COMPRESSION_ALGORITHMS); /* comp c->s */
    pos += put_namelist(out + pos, COMPRESSION_ALGORITHMS); /* comp s->c */
    pos += put_namelist(out + pos, LANGUAGES);              /* lang c->s */
    pos += put_namelist(out + pos, LANGUAGES);              /* lang s->c */

    out[pos++] = 0; /* first_kex_packet_follows: false --
                      * we're not speculatively sending a guessed KEX
                      * packet right behind this one */
    put_u32(out + pos, 0); pos += 4; /* reserved */

    return pos;
}

/* Reads one RFC 4253 name-list at `data`+`*pos` (bounded by `len`),
 * advances *pos past it, and returns a pointer to its (non-NUL-
 * terminated) content plus its length in *out_len. Returns NULL on a
 * truncated/malformed list. */
static const uint8_t *read_namelist(const uint8_t *data, uint32_t len,
                                     uint32_t *pos, uint32_t *out_len) {
    if (*pos + 4 > len) return NULL;
    uint32_t n = get_u32(data + *pos);
    *pos += 4;
    if (*pos + n > len) return NULL;
    const uint8_t *start = data + *pos;
    *pos += n;
    *out_len = n;
    return start;
}

static void print_namelist(const char *label, const uint8_t *list, uint32_t len) {
    static char buf[512];
    uint32_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    mcopy(buf, list, n);
    buf[n] = 0;
    println("");
    print("[SSHD] peer ");
    print(label);
    print(": ");
    println(buf);
}

/* Exact-token match against a comma-separated name-list (not a bare
 * substring search -- "ssh-ed25519" must not match inside
 * "ssh-ed25519-cert-v01@openssh.com"). */
static int namelist_contains(const uint8_t *list, uint32_t len, const char *needle) {
    uint32_t needle_len = (uint32_t)slen(needle);
    uint32_t start = 0;

    for (uint32_t i = 0; i <= len; i++) {
        if (i == len || list[i] == ',') {
            uint32_t tok_len = i - start;
            if (tok_len == needle_len && mcompare(list + start, needle, needle_len) == 0)
                return 1;
            start = i + 1;
        }
    }
    return 0;
}

int ssh_kexinit_check(const uint8_t *data, uint32_t len) {
    uint32_t pos = 0;
    const uint8_t *lists[10];
    uint32_t list_lens[10];
    static const char *labels[10] = {
        "kex_algorithms", "server_host_key_algorithms",
        "encryption_algorithms_client_to_server",
        "encryption_algorithms_server_to_client",
        "mac_algorithms_client_to_server", "mac_algorithms_server_to_client",
        "compression_algorithms_client_to_server",
        "compression_algorithms_server_to_client",
        "languages_client_to_server", "languages_server_to_client"
    };

    if (len < 1 + 16) return 0;
    if (data[0] != SSH_MSG_KEXINIT) return 0;
    pos = 1 + 16; /* skip type byte + cookie */

    for (int i = 0; i < 10; i++) {
        lists[i] = read_namelist(data, len, &pos, &list_lens[i]);
        if (!lists[i]) {
            println("[SSHD] malformed KEXINIT (truncated name-list)");
            return 0;
        }
        print_namelist(labels[i], lists[i], list_lens[i]);
    }
    /* first_kex_packet_follows (1 byte) + reserved (4 bytes) follow
     * here but aren't needed for this negotiation check. */

    int ok_kex    = namelist_contains(lists[0], list_lens[0], KEX_ALGORITHMS);
    int ok_hostkey= namelist_contains(lists[1], list_lens[1], HOST_KEY_ALGORITHMS);
    int ok_cipher_cs = namelist_contains(lists[2], list_lens[2], CIPHER_ALGORITHMS);
    int ok_cipher_sc = namelist_contains(lists[3], list_lens[3], CIPHER_ALGORITHMS);

    print("[SSHD] required algorithms offered by peer: kex=");
    print(ok_kex ? "yes" : "NO");
    print(" host_key=");
    print(ok_hostkey ? "yes" : "NO");
    print(" cipher_c2s=");
    print(ok_cipher_cs ? "yes" : "NO");
    print(" cipher_s2c=");
    println(ok_cipher_sc ? "yes" : "NO");

    return ok_kex && ok_hostkey && ok_cipher_cs && ok_cipher_sc;
}