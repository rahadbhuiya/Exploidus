/*
 * ed25519.c -- Ed25519 signatures (RFC 8032), ported from TweetNaCl
 * (https://tweetnacl.cr.yp.to/, version 20131229)'s crypto_sign_*
 * functions -- Daniel J. Bernstein, Bart Mennink, Bernard van
 * Gastel, Wesley Janssen, Tanja Lange, Peter Schwabe, Sjaak
 * Smetsers -- public domain.
 *
 * Chosen for the same reason as this project's other ported crypto:
 * the twisted-Edwards field arithmetic and point operations below
 * (the gf type, M/S/A/Z/inv25519/pow2523, the Edwards point
 * add/scalarmult/pack) are exactly the kind of code that's correct
 * by construction in a reviewed reference and easy to break subtly
 * by "cleaning up" -- TweetNaCl in particular was written and
 * published specifically to be small enough to audit by hand while
 * still being a complete, correct NaCl-compatible implementation
 * (see the TweetNaCl paper, "A Crypto Library in 100 Tweets").
 *
 * Adapted from TweetNaCl's original API (which bundles the message
 * and signature into one buffer -- crypto_sign() outputs
 * signature||message, crypto_sign_open() verifies and strips the
 * signature back off) to a detached-signature shape
 * (ed25519_sign()/ed25519_verify(), matching ed25519.h) since that's
 * what SSH's wire format actually needs -- a signature is its own
 * field, sent alongside the data it covers, not concatenated with
 * it. This changes buffer layout/bookkeeping only, not the
 * arithmetic: every field-element/point operation below (M, S, A, Z,
 * inv25519, pow2523, the Edwards add/scalarmult/scalarbase/pack, and
 * modL's mod-L scalar reduction) is unmodified from TweetNaCl's
 * originals.
 *
 * Also adapted: crypto_sign_keypair() called randombytes() itself;
 * ed25519_keypair_from_seed() below takes the seed as a parameter
 * instead, so this file has no RNG dependency (portable/host-
 * testable, and lets a caller control/persist the seed for a host
 * key deliberately, rather than always generating a fresh random
 * one). Uses this project's own sha512.c (itself extracted from this
 * same TweetNaCl source -- see sha512.c) rather than TweetNaCl's
 * crypto_hash, which is the same function under a different name.
 *
 * Verified against RFC 8032 Section 7.1's Ed25519 test vectors
 * before being written into the kernel tree -- see the project
 * README for the verification transcript.
 */

#include <stdint.h>
#include "ed25519.h"
#include "sha512.h"

typedef uint8_t u8;
typedef int64_t i64;
typedef i64 gf[16];

static const gf
    gf0 = {0},
    gf1 = {1},
    D  = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203},
    D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406},
    X  = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169},
    Y  = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666},
    I  = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

static int vn(const u8 *x, const u8 *y, int n) {
    int i; uint32_t d = 0;
    for (i = 0; i < n; i++) d |= x[i] ^ y[i];
    return (1 & ((d - 1) >> 8)) - 1;
}
static int crypto_verify_32(const u8 *x, const u8 *y) { return vn(x, y, 32); }

static void set25519(gf r, const gf a) { int i; for (i = 0; i < 16; i++) r[i] = a[i]; }

static void car25519(gf o) {
    int i; i64 c;
    for (i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b) {
    i64 t, i, c = ~(b - 1);
    for (i = 0; i < 16; i++) { t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}

static void pack25519(u8 *o, const gf n) {
    int i, j, b;
    gf m, t;
    for (i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (m[15] >> 16) & 1;
        m[15] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) { o[2 * i] = (u8)(t[i] & 0xff); o[2 * i + 1] = (u8)(t[i] >> 8); }
}

static int neq25519(const gf a, const gf b) {
    u8 c[32], d[32];
    pack25519(c, a); pack25519(d, b);
    return crypto_verify_32(c, d);
}

static u8 par25519(const gf a) { u8 d[32]; pack25519(d, a); return d[0] & 1; }

static void unpack25519(gf o, const u8 *n) {
    int i;
    for (i = 0; i < 16; i++) o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void Z(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void M(gf o, const gf a, const gf b) {
    i64 i, j, t[31];
    for (i = 0; i < 31; i++) t[i] = 0;
    for (i = 0; i < 16; i++) for (j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

static void inv25519(gf o, const gf i_) {
    gf c; int a;
    for (a = 0; a < 16; a++) c[a] = i_[a];
    for (a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i_);
    }
    for (a = 0; a < 16; a++) o[a] = c[a];
}

static void pow2523(gf o, const gf i_) {
    gf c; int a;
    for (a = 0; a < 16; a++) c[a] = i_[a];
    for (a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1) M(c, c, i_);
    }
    for (a = 0; a < 16; a++) o[a] = c[a];
}

/* Twisted-Edwards point addition, p += q (extended coordinates). */
static void edwards_add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]); Z(t, q[1], q[0]); M(a, a, t);
    A(b, p[0], p[1]); A(t, q[0], q[1]); M(b, b, t);
    M(c, p[3], q[3]); M(c, c, D2);
    M(d, p[2], q[2]); A(d, d, d);
    Z(e, b, a); Z(f, d, c); A(g, d, c); A(h, b, a);
    M(p[0], e, f); M(p[1], h, g); M(p[2], g, f); M(p[3], e, h);
}

static void cswap(gf p[4], gf q[4], u8 b) {
    int i; for (i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

static void edwards_pack(u8 *r, gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi); M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (u8)(par25519(tx) << 7);
}

static void edwards_scalarmult(gf p[4], gf q[4], const u8 *s) {
    int i;
    set25519(p[0], gf0); set25519(p[1], gf1); set25519(p[2], gf1); set25519(p[3], gf0);
    for (i = 255; i >= 0; i--) {
        u8 b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        edwards_add(q, p);
        edwards_add(p, p);
        cswap(p, q, b);
    }
}

static void edwards_scalarbase(gf p[4], const u8 *s) {
    gf q[4];
    set25519(q[0], X); set25519(q[1], Y); set25519(q[2], gf1); M(q[3], X, Y);
    edwards_scalarmult(p, q, s);
}

/* L: the order of the Ed25519 base point's subgroup. */
static const u8 L[32] = {
    0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10
};

static void modL(u8 *r, i64 x[64]) {
    i64 carry, i, j;
    for (i = 63; i >= 32; i--) {
        carry = 0;
        for (j = i - 32; j < i - 12; j++) {
            x[j] += carry - 16 * x[i] * L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; j++) x[j] -= carry * L[j];
    for (i = 0; i < 32; i++) { x[i + 1] += x[i] >> 8; r[i] = (u8)(x[i] & 255); }
}

static void reduce(u8 *r) {
    i64 x[64], i;
    for (i = 0; i < 64; i++) x[i] = r[i];
    for (i = 0; i < 64; i++) r[i] = 0;
    modL(r, x);
}

void ed25519_keypair_from_seed(u8 public_key[ED25519_PUBLIC_KEY_LEN],
                                u8 secret_key[ED25519_SECRET_KEY_LEN],
                                const u8 seed[ED25519_SEED_LEN]) {
    u8 d[64];
    gf p[4];
    int i;

    for (i = 0; i < 32; i++) secret_key[i] = seed[i];

    sha512(secret_key, 32, d);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;

    edwards_scalarbase(p, d);
    edwards_pack(public_key, p);

    for (i = 0; i < 32; i++) secret_key[32 + i] = public_key[i];
}

/*
 * Scratch buffer for combining fields before hashing (prefix||message
 * for the nonce hash, and R||A||message for the challenge hash).
 * Static and bounded rather than heap-allocated or an unbounded VLA,
 * same reasoning as chacha20poly1305.c's MAC_DATA_MAX: this is meant
 * to run inside sshd, which handles one connection/signing operation
 * at a time, so per-process static state here isn't a cross-request
 * hazard (see chacha20poly1305.c's own comment for the fuller
 * argument, and the netbuf-pool-race section of the project README
 * for why that distinction matters at all).
 */
#define ED25519_SCRATCH_MAX (40 * 1024 + 128)
static u8 g_scratch[ED25519_SCRATCH_MAX];

void ed25519_sign(u8 signature[ED25519_SIGNATURE_LEN],
                   const u8 *message, size_t message_len,
                   const u8 secret_key[ED25519_SECRET_KEY_LEN]) {
    u8 d[64], h[64], r[64];
    i64 i, j, x[64];
    gf p[4];

    /* Message too large for the scratch buffer: this is a hard
     * failure mode a caller must not ignore. Zeroing the signature
     * makes that failure loud (it will never verify) rather than
     * silently truncating the signed data. */
    if (message_len > ED25519_SCRATCH_MAX - 64) {
        for (i = 0; i < 64; i++) signature[i] = 0;
        return;
    }

    sha512(secret_key, 32, d);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;

    /* r = SHA-512(prefix || message) mod L, where prefix = d[32..63] */
    for (i = 0; i < 32; i++) g_scratch[i] = d[32 + i];
    for (i = 0; i < (i64)message_len; i++) g_scratch[32 + i] = message[i];
    sha512(g_scratch, 32 + message_len, r);
    reduce(r);

    /* R = r*B, pack into signature[0..31] */
    edwards_scalarbase(p, r);
    edwards_pack(signature, p);

    /* h = SHA-512(R || A || message) mod L, where A = public key
     * (secret_key[32..63]) */
    for (i = 0; i < 32; i++) g_scratch[i] = signature[i];
    for (i = 0; i < 32; i++) g_scratch[32 + i] = secret_key[32 + i];
    for (i = 0; i < (i64)message_len; i++) g_scratch[64 + i] = message[i];
    sha512(g_scratch, 64 + message_len, h);
    reduce(h);

    /* S = (r + h*a) mod L, where a = d[0..31] (the clamped scalar) */
    for (i = 0; i < 64; i++) x[i] = 0;
    for (i = 0; i < 32; i++) x[i] = r[i];
    for (i = 0; i < 32; i++) for (j = 0; j < 32; j++) x[i + j] += h[i] * (i64)d[j];
    modL(signature + 32, x);
}

static int unpackneg(gf r[4], const u8 p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);

    S(den2, den); S(den4, den2); M(den6, den4, den2);
    M(t, den6, num); M(t, t, den);

    pow2523(t, t);
    M(t, t, num); M(t, t, den); M(t, t, den);
    M(r[0], t, den);

    S(chk, r[0]); M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], I);

    S(chk, r[0]); M(chk, chk, den);
    if (neq25519(chk, num)) return -1;

    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);

    M(r[3], r[0], r[1]);
    return 0;
}

int ed25519_verify(const u8 signature[ED25519_SIGNATURE_LEN],
                    const u8 *message, size_t message_len,
                    const u8 public_key[ED25519_PUBLIC_KEY_LEN]) {
    u8 h[64];
    gf p[4], q[4];
    i64 i;

    if (message_len > ED25519_SCRATCH_MAX - 64)
        return 0; /* can't verify -> fail closed, same convention as
                    * chacha20poly1305_decrypt() */

    if (unpackneg(q, public_key)) return 0;

    for (i = 0; i < 32; i++) g_scratch[i] = signature[i];
    for (i = 0; i < 32; i++) g_scratch[32 + i] = public_key[i];
    for (i = 0; i < (i64)message_len; i++) g_scratch[64 + i] = message[i];
    sha512(g_scratch, 64 + message_len, h);
    reduce(h);

    edwards_scalarmult(p, q, h);
    edwards_scalarbase(q, signature + 32);
    edwards_add(p, q);

    u8 t[32];
    edwards_pack(t, p);

    return crypto_verify_32(signature, t) == 0;
}