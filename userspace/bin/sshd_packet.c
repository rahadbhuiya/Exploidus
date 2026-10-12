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
#include "../../kernel/crypto/chacha20.h"
#include "../../kernel/crypto/poly1305.h"

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

static int plain_send(cap_token_t cap, int fd, const uint8_t *payload,
                      uint32_t payload_len) {
    static uint8_t buf[SSH_PACKET_MAX + 64];
    const uint32_t block_size = 8; /* unencrypted minimum, RFC 4253 Section 6 */

    if (payload_len > SSH_PACKET_MAX)
        return 0;

    /*
     * BUG FIX: RFC 4253 Section 6 requires "the length of the
     * concatenation of 'packet_length', 'padding_length', 'payload',
     * and 'random padding'" -- i.e. the FULL on-wire size INCLUDING
     * the 4-byte packet_length field itself -- to be a multiple of
     * the block size. This originally only aligned
     * (padding_length + payload + padding), which is packet_length's
     * own VALUE, missing the +4 for the length field that precedes
     * it on the wire. A real OpenSSH client correctly rejected the
     * result: "padding error: need N block 8 mod 4" -- the "mod 4"
     * there is exactly this off-by-4.
     */
    uint32_t base = 4 + 1 + payload_len; /* length field + padding_length byte + payload */
    uint32_t pad = block_size - (base % block_size);
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

static int32_t plain_recv(cap_token_t cap, int fd, uint8_t *out) {
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

/* ---- chacha20-poly1305@openssh.com packet layer ----
 *
 * OpenSSH's construction (PROTOCOL.chacha20poly1305), NOT RFC 8439:
 *   - 64 bytes of key per direction: K_2 = key[0..31] (main: payload
 *     and the one-time Poly1305 key), K_1 = key[32..63] (header: the
 *     4-byte length).
 *   - The nonce is the 32-bit packet sequence number as an 8-byte
 *     big-endian value. OpenSSH uses the original ChaCha layout (64-bit
 *     block counter, 64-bit nonce); with the counter's high word zero
 *     that is exactly the RFC 8439 layout with the 12-byte nonce
 *     00 00 00 00 || seqno8, which is what chacha20.c implements.
 *   - Poly1305 key = first 32 bytes of K_2's block 0; the payload is
 *     encrypted with K_2 from block 1; the length with K_1 from block 0.
 *   - Tag = Poly1305 over (encrypted length || encrypted body), 16
 *     bytes, appended. Wire: enc(len)[4] || enc(body)[len] || tag[16].
 *   - packet_length is NOT included in the padding alignment: it
 *     is (padding_length byte + payload + padding), a multiple of 8,
 *     with at least 4 bytes of padding.
 * The tag is verified BEFORE anything is decrypted or parsed.
 */

static void chacha_nonce(uint8_t n[12], uint32_t seq) {
    for (int i = 0; i < 8; i++) n[i] = 0;
    put_u32(n + 8, seq);
}

static int ct_equal16(const uint8_t *a, const uint8_t *b) {
    uint8_t acc = 0;
    for (int i = 0; i < 16; i++) acc |= (uint8_t)(a[i] ^ b[i]);
    return acc == 0;
}

static void wipe(void *p, uint32_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static int enc_send(ssh_transport_t *t, const uint8_t *payload, uint32_t payload_len) {
    static uint8_t buf[4 + SSH_PACKET_MAX + 16];
    uint8_t nonce[12], blk[64];

    uint32_t pad = 8 - ((1 + payload_len) % 8);
    if (pad < 4) pad += 8;
    uint32_t packet_length = 1 + payload_len + pad;     /* multiple of 8 */
    if (payload_len > SSH_PACKET_MAX || packet_length > SSH_PACKET_MAX)
        return 0;

    put_u32(buf, packet_length);
    buf[4] = (uint8_t)pad;
    mcopy(buf + 5, payload, payload_len);
    {
        uint8_t padbuf[16];
        if (getrandom(padbuf, (uint64_t)pad) < 0) return 0;
        mcopy(buf + 5 + payload_len, padbuf, pad);
    }

    chacha_nonce(nonce, t->seq_send);
    chacha20_block(blk, t->key_send, 0, nonce);                    /* Poly1305 key: blk[0..31] */
    chacha20_xor(buf + 4, buf + 4, packet_length, t->key_send, 1, nonce);
    chacha20_xor(buf, buf, 4, t->key_send + 32, 0, nonce);         /* length, header key */
    poly1305_auth(buf + 4 + packet_length, buf, 4 + packet_length, blk);
    wipe(blk, sizeof(blk));

    return send_all(t->cap, t->fd, buf, 4 + packet_length + 16);
}

static int32_t enc_recv(ssh_transport_t *t, uint8_t *out) {
    static uint8_t buf[4 + SSH_PACKET_MAX + 16];
    uint8_t nonce[12], blk[64], lenp[4], tag[16];

    if (!recv_exact(t->cap, t->fd, buf, 4)) return -1;
    chacha_nonce(nonce, t->seq_recv);
    mcopy(lenp, buf, 4);
    chacha20_xor(lenp, lenp, 4, t->key_recv + 32, 0, nonce);
    uint32_t packet_length = get_u32(lenp);
    /* Smallest legal: padding_length(1) + payload(1) + padding(4) = 6,
     * rounded up to the block size 8. */
    if (packet_length < 8 || packet_length > SSH_PACKET_MAX || (packet_length % 8) != 0)
        return -1;

    if (!recv_exact(t->cap, t->fd, buf + 4, packet_length + 16)) return -1;

    chacha20_block(blk, t->key_recv, 0, nonce);
    poly1305_auth(tag, buf, 4 + packet_length, blk);
    wipe(blk, sizeof(blk));
    if (!ct_equal16(tag, buf + 4 + packet_length)) {
        println("[SSHD] packet authentication FAILED (bad Poly1305 tag), dropping connection");
        return -1;
    }

    chacha20_xor(buf + 4, buf + 4, packet_length, t->key_recv, 1, nonce);
    uint32_t pad = buf[4];
    if (pad < 4 || pad + 1 >= packet_length) return -1;      /* also: payload >= 1 byte */
    uint32_t payload_len = packet_length - pad - 1;
    mcopy(out, buf + 5, payload_len);
    return (int32_t)payload_len;
}

/* ---- transport: sequence numbers + plaintext/encrypted dispatch ---- */

void ssh_transport_init(ssh_transport_t *t, cap_token_t cap, int fd) {
    wipe(t, sizeof(*t));
    t->cap = cap;
    t->fd = fd;
}

int ssh_send(ssh_transport_t *t, const uint8_t *payload, uint32_t payload_len) {
    int ok = t->enc_send ? enc_send(t, payload, payload_len)
                         : plain_send(t->cap, t->fd, payload, payload_len);
    if (ok) t->seq_send++;
    return ok;
}

int32_t ssh_recv(ssh_transport_t *t, uint8_t *out) {
    int32_t n = t->enc_recv ? enc_recv(t, out)
                            : plain_recv(t->cap, t->fd, out);
    if (n >= 0) t->seq_recv++;
    return n;
}

void ssh_transport_start_send_crypto(ssh_transport_t *t, const uint8_t key[64]) {
    mcopy(t->key_send, key, 64);
    t->enc_send = 1;
    if (t->strict) t->seq_send = 0;      /* kex-strict: reset on NEWKEYS */
}

void ssh_transport_start_recv_crypto(ssh_transport_t *t, const uint8_t key[64]) {
    mcopy(t->key_recv, key, 64);
    t->enc_recv = 1;
    if (t->strict) t->seq_recv = 0;
}

void ssh_transport_wipe(ssh_transport_t *t) {
    wipe(t, sizeof(*t));
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
#define KEX_STRICT_S        "kex-strict-s-v00@openssh.com" /* Terrapin mitigation, advertised */
#define KEX_STRICT_C        "kex-strict-c-v00@openssh.com"
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

    pos += put_namelist(out + pos, KEX_ALGORITHMS "," KEX_STRICT_S);
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

/* True if the FIRST comma-separated entry of the list equals needle. */
static int namelist_first_is(const uint8_t *list, uint32_t len, const char *needle) {
    uint32_t needle_len = (uint32_t)slen(needle);
    uint32_t tok = 0;
    while (tok < len && list[tok] != ',') tok++;
    return tok == needle_len && mcompare(list, needle, needle_len) == 0;
}

int ssh_kexinit_wrong_guess(const uint8_t *data, uint32_t len) {
    uint32_t pos = 1 + 16;
    const uint8_t *kex = NULL, *hostkey = NULL;
    uint32_t kex_len = 0, hostkey_len = 0;

    if (len < pos || data[0] != SSH_MSG_KEXINIT) return 0;
    for (int i = 0; i < 10; i++) {
        uint32_t n = 0;
        const uint8_t *l = read_namelist(data, len, &pos, &n);
        if (!l) return 0;
        if (i == 0) { kex = l; kex_len = n; }
        if (i == 1) { hostkey = l; hostkey_len = n; }
    }
    if (pos + 1 > len) return 0;
    if (data[pos] == 0) return 0;   /* first_kex_packet_follows not set */

    if (namelist_first_is(kex, kex_len, KEX_ALGORITHMS) &&
        namelist_first_is(hostkey, hostkey_len, HOST_KEY_ALGORITHMS))
        return 0;                   /* guess matches our choice: it is the real KEX packet */
    return 1;
}

int ssh_kexinit_peer_strict(const uint8_t *data, uint32_t len) {
    uint32_t pos = 1 + 16, n = 0;
    if (len < pos || data[0] != SSH_MSG_KEXINIT) return 0;
    const uint8_t *kex = read_namelist(data, len, &pos, &n);
    return kex && namelist_contains(kex, n, KEX_STRICT_C);
}