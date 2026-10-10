/*
 * sshd_kex.c -- server side of curve25519-sha256 key exchange
 * (RFC 8731) with an ssh-ed25519 host key (RFC 8709), wiring together
 * kernel/crypto/{x25519,sha256,ed25519}.c.
 *
 * Exchange hash (RFC 8731 Section 3.1 / RFC 4253 Section 8):
 *
 *   H = SHA256( string V_C || string V_S || string I_C || string I_S ||
 *               string K_S || string Q_C || string Q_S || mpint K )
 *
 * V_* are the identification strings without CR LF, I_* the full
 * KEXINIT payloads (type byte included), K_S the host key blob, Q_*
 * the raw 32-byte X25519 public values, K the shared secret.
 *
 * About K's encoding: X25519 returns 32 bytes. Like OpenSSH
 * (kexc25519.c: sshbuf_put_bignum2_bytes on the raw output), those
 * bytes are used as a BIG-endian integer WITHOUT reversing them, then
 * encoded as an SSH mpint (leading zero bytes stripped, a 0x00 prefix
 * if the top bit is set). This is the one place a "natural"
 * little-endian reading of RFC 7748 would silently break interop: both
 * sides would still agree with themselves but never with a real peer.
 *
 * Key derivation is RFC 4253 Section 7.2 with SHA-256:
 *   K1 = HASH(K || H || X || session_id), K2 = HASH(K || H || K1), ...
 * Only letters 'C' and 'D' are derived (the two cipher keys): an AEAD
 * cipher has no IV or integrity key ('A','B','E','F' are unused).
 *
 * Written from the RFC field lists, same approach as sshd_packet.c:
 * it's framing and bookkeeping, and the primitives it calls are
 * already verified against their own vectors.
 */

#include <stdint.h>
#include <stddef.h>
#include "../libc/syscall.h"
#include "sshd_packet.h"
#include "sshd_kex.h"
#include "../../kernel/crypto/x25519.h"
#include "../../kernel/crypto/sha256.h"
#include "../../kernel/crypto/ed25519.h"

#define SSH_MSG_DISCONNECT  1
#define SSH_MSG_IGNORE      2
#define SSH_MSG_DEBUG       4

#define HOSTKEY_ALG "ssh-ed25519"

static void println(const char *s) { puts(s); putc('\n'); }

/* Local helpers instead of <string.h> -- see the comment in
 * sshd_packet.c on why this file avoids it. */
static void kx_copy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

/* Zeroization the compiler may not elide (volatile stores). */
static void kx_wipe(void *p, uint32_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static int kx_all_zero(const uint8_t *p, uint32_t n) {
    uint8_t acc = 0;
    for (uint32_t i = 0; i < n; i++) acc |= p[i];
    return acc == 0;
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)(v);
}

static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* Appends `string` (uint32 length + bytes). Returns bytes written. */
static uint32_t put_string(uint8_t *out, const uint8_t *data, uint32_t n) {
    put_u32(out, n);
    kx_copy(out + 4, data, n);
    return 4 + n;
}

/* Encodes the 32 raw X25519 output bytes as an SSH mpint (see header
 * comment for the byte-order rationale). out must hold >= 37 bytes.
 * Returns the total encoded length including the 4-byte length. */
static uint32_t mpint_from_be32(uint8_t *out, const uint8_t k[32]) {
    uint32_t i = 0;
    while (i < 32 && k[i] == 0) i++;
    uint32_t n = 32 - i;
    uint32_t pad = (n > 0 && (k[i] & 0x80)) ? 1 : 0;
    put_u32(out, n + pad);
    uint32_t pos = 4;
    if (pad) out[pos++] = 0;
    kx_copy(out + pos, k + i, n);
    return pos + n;
}

/* ---- streaming exchange-hash helpers ---- */

static void h_string(sha256_ctx_t *c, const uint8_t *d, uint32_t n) {
    uint8_t len[4];
    put_u32(len, n);
    sha256_update(c, len, 4);
    sha256_update(c, d, n);
}

/* ---- host key blob ---- */

uint32_t ssh_hostkey_blob(uint8_t *out, const uint8_t public_key[32]) {
    uint32_t pos = 0;
    pos += put_string(out + pos, (const uint8_t *)HOSTKEY_ALG, 11);
    pos += put_string(out + pos, public_key, 32);
    return pos;
}

/* ---- key derivation (RFC 4253 Section 7.2) ---- */

static void derive_key(uint8_t *out, uint32_t outlen,
                       const uint8_t *kmp, uint32_t kmp_len,
                       const uint8_t H[32], uint8_t letter,
                       const uint8_t session_id[32]) {
    sha256_ctx_t c;
    uint8_t blk[SHA256_DIGEST_LEN];
    uint32_t have = 0;

    sha256_init(&c);
    sha256_update(&c, kmp, kmp_len);
    sha256_update(&c, H, 32);
    sha256_update(&c, &letter, 1);
    sha256_update(&c, session_id, 32);
    sha256_final(&c, blk);
    {
        uint32_t take = outlen < 32 ? outlen : 32;
        kx_copy(out, blk, take);
        have = take;
    }
    while (have < outlen) {
        sha256_init(&c);
        sha256_update(&c, kmp, kmp_len);
        sha256_update(&c, H, 32);
        sha256_update(&c, out, have);   /* K1 || ... || K(n-1) */
        sha256_final(&c, blk);
        uint32_t take = (outlen - have) < 32 ? (outlen - have) : 32;
        kx_copy(out + have, blk, take);
        have += take;
    }
    kx_wipe(blk, sizeof(blk));
}

/* ---- packet receive that tolerates IGNORE/DEBUG (RFC 4253 11) ---- */

static int32_t recv_kex_packet(cap_token_t cap, int fd, uint8_t *buf) {
    for (int i = 0; i < 16; i++) {
        int32_t n = ssh_packet_recv(cap, fd, buf);
        if (n <= 0) return -1;
        if (buf[0] == SSH_MSG_IGNORE || buf[0] == SSH_MSG_DEBUG)
            continue;
        if (buf[0] == SSH_MSG_DISCONNECT) {
            println("[SSHD] peer sent SSH_MSG_DISCONNECT during key exchange");
            return -1;
        }
        return n;
    }
    return -1;
}

int ssh_kex_ecdh_server(cap_token_t cap, int fd,
                        const uint8_t *v_c, uint32_t v_c_len,
                        const uint8_t *v_s, uint32_t v_s_len,
                        const uint8_t *i_c, uint32_t i_c_len,
                        const uint8_t *i_s, uint32_t i_s_len,
                        const ssh_hostkey_t *hk, ssh_keys_t *out) {
    static uint8_t rx[SSH_PACKET_MAX];
    uint8_t q_c[32], q_s[32], eph[32], k[32];
    uint8_t ks[64], sig[ED25519_SIGNATURE_LEN];
    uint8_t sigblob[4 + 11 + 4 + ED25519_SIGNATURE_LEN];
    uint8_t reply[1 + (4 + 51) + (4 + 32) + (4 + sizeof(sigblob))];
    uint8_t kmp[40];
    uint8_t H[SHA256_DIGEST_LEN];
    uint32_t ks_len, kmp_len, pos;
    int ok = 0;

    kx_wipe(out, sizeof(*out));

    /* 1. SSH_MSG_KEX_ECDH_INIT: byte 30, string Q_C (exactly 32). */
    println("[SSHD] waiting for SSH_MSG_KEX_ECDH_INIT...");
    int32_t n = recv_kex_packet(cap, fd, rx);
    if (n < 0) {
        println("[SSHD] no KEX_ECDH_INIT received");
        return 0;
    }
    if (rx[0] != SSH_MSG_KEX_ECDH_INIT) {
        println("[SSHD] expected KEX_ECDH_INIT, got a different message type");
        return 0;
    }
    if (n != 1 + 4 + 32 || get_u32(rx + 1) != 32) {
        println("[SSHD] malformed KEX_ECDH_INIT (Q_C must be a 32-byte string)");
        return 0;
    }
    kx_copy(q_c, rx + 5, 32);   /* rx is reused for the client's NEWKEYS */

    /* 2. Ephemeral X25519 key pair + shared secret. */
    if (getrandom(eph, 32) < 0 || kx_all_zero(eph, 32)) {
        println("[SSHD] no usable randomness for the ephemeral key, aborting");
        goto done;
    }
    x25519_scalarmult(q_s, eph, X25519_BASEPOINT);
    x25519_scalarmult(k, eph, q_c);
    kx_wipe(eph, sizeof(eph));

    /* RFC 7748 Section 6.1: a low-order peer point yields an all-zero
     * output; the exchange must be aborted (OpenSSH does the same). */
    if (kx_all_zero(k, 32)) {
        println("[SSHD] rejecting KEX_ECDH_INIT: low-order public key (all-zero secret)");
        goto done;
    }

    /* 3. Exchange hash H. */
    ks_len = ssh_hostkey_blob(ks, hk->public_key);
    kmp_len = mpint_from_be32(kmp, k);
    {
        sha256_ctx_t c;
        sha256_init(&c);
        h_string(&c, v_c, v_c_len);
        h_string(&c, v_s, v_s_len);
        h_string(&c, i_c, i_c_len);
        h_string(&c, i_s, i_s_len);
        h_string(&c, ks, ks_len);
        h_string(&c, q_c, 32);
        h_string(&c, q_s, 32);
        sha256_update(&c, kmp, kmp_len);
        sha256_final(&c, H);
    }

    /* 4. Sign H with the host key (ssh-ed25519 signs the 32-byte hash
     * itself, RFC 8709 Section 6) and build the signature blob. */
    ed25519_sign(sig, H, sizeof(H), hk->secret_key);
    pos = 0;
    pos += put_string(sigblob + pos, (const uint8_t *)HOSTKEY_ALG, 11);
    pos += put_string(sigblob + pos, sig, ED25519_SIGNATURE_LEN);

    /* 5. SSH_MSG_KEX_ECDH_REPLY, then our SSH_MSG_NEWKEYS. */
    {
        uint32_t rp = 0;
        reply[rp++] = SSH_MSG_KEX_ECDH_REPLY;
        rp += put_string(reply + rp, ks, ks_len);
        rp += put_string(reply + rp, q_s, 32);
        rp += put_string(reply + rp, sigblob, pos);
        println("[SSHD] sending SSH_MSG_KEX_ECDH_REPLY...");
        if (!ssh_packet_send(cap, fd, reply, rp)) {
            println("[SSHD] failed to send KEX_ECDH_REPLY");
            goto done;
        }
    }
    {
        const uint8_t newkeys = SSH_MSG_NEWKEYS;
        println("[SSHD] sending SSH_MSG_NEWKEYS...");
        if (!ssh_packet_send(cap, fd, &newkeys, 1)) {
            println("[SSHD] failed to send NEWKEYS");
            goto done;
        }
    }

    /* 6. The client's NEWKEYS tells us it has switched; nothing else
     * is legal between its KEX_ECDH_INIT and this. */
    println("[SSHD] waiting for the client's SSH_MSG_NEWKEYS...");
    n = recv_kex_packet(cap, fd, rx);
    if (n != 1 || rx[0] != SSH_MSG_NEWKEYS) {
        println("[SSHD] expected a bare SSH_MSG_NEWKEYS from the client");
        goto done;
    }

    /* 7. Derive the two cipher keys; first KEX, so session_id = H. */
    kx_copy(out->session_id, H, 32);
    derive_key(out->key_c2s, 64, kmp, kmp_len, H, 'C', out->session_id);
    derive_key(out->key_s2c, 64, kmp, kmp_len, H, 'D', out->session_id);
    ok = 1;

done:
    kx_wipe(eph, sizeof(eph));
    kx_wipe(k, sizeof(k));
    kx_wipe(kmp, sizeof(kmp));
    kx_wipe(sig, sizeof(sig));
    if (!ok) kx_wipe(out, sizeof(*out));
    return ok;
}