/*
 * sha512.c -- SHA-512 (FIPS 180-4), extracted from TweetNaCl
 * (https://tweetnacl.cr.yp.to/, version 20131229), by Daniel J.
 * Bernstein, Bart Mennink, Bernard van Gastel, Wesley Janssen, Tanja
 * Lange, Peter Schwabe, Sjaak Smetsers -- public domain.
 *
 * Needed as Ed25519's internal hash function (ed25519.c) -- RFC 8032
 * specifies SHA-512 throughout, distinct from the SHA-256 this
 * kernel also has (sha256.c, for RFC 8731's curve25519-sha256 KEX
 * hash instead).
 *
 * Only cosmetic changes from TweetNaCl's crypto_hashblocks/
 * crypto_hash: renamed to sha512()/sha512_blocks(), u8/u64/i64
 * typedefs replaced with this kernel's uint8_t/uint64_t/int64_t,
 * `FOR(i,n)` macro expanded to plain for-loops, comments added. No
 * arithmetic changed -- the round constants, message schedule, and
 * compression function are unmodified from the original.
 */

#include <stdint.h>
#include "sha512.h"

static uint64_t dl64(const uint8_t *x) {
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u = (u << 8) | x[i];
    return u;
}

static void ts64(uint8_t *x, uint64_t u) {
    for (int i = 7; i >= 0; --i) { x[i] = (uint8_t)u; u >>= 8; }
}

static uint64_t R(uint64_t x, int c) { return (x >> c) | (x << (64 - c)); }
static uint64_t Ch(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (~x & z); }
static uint64_t Maj(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static uint64_t Sigma0(uint64_t x) { return R(x,28) ^ R(x,34) ^ R(x,39); }
static uint64_t Sigma1(uint64_t x) { return R(x,14) ^ R(x,18) ^ R(x,41); }
static uint64_t sigma0(uint64_t x) { return R(x, 1) ^ R(x, 8) ^ (x >> 7); }
static uint64_t sigma1(uint64_t x) { return R(x,19) ^ R(x,61) ^ (x >> 6); }

static const uint64_t K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

/* Processes as many full 128-byte blocks as fit in n bytes of m,
 * updating the 64-byte state x in place. Returns the number of
 * trailing bytes NOT consumed (n % 128), same convention TweetNaCl's
 * crypto_hashblocks used. */
static uint64_t sha512_blocks(uint8_t *x, const uint8_t *m, uint64_t n) {
    uint64_t z[8], b[8], a[8], w[16], t;
    int i, j;

    for (i = 0; i < 8; i++) z[i] = a[i] = dl64(x + 8 * i);

    while (n >= 128) {
        for (i = 0; i < 16; i++) w[i] = dl64(m + 8 * i);

        for (i = 0; i < 80; i++) {
            for (j = 0; j < 8; j++) b[j] = a[j];
            t = a[7] + Sigma1(a[4]) + Ch(a[4],a[5],a[6]) + K[i] + w[i % 16];
            b[7] = t + Sigma0(a[0]) + Maj(a[0],a[1],a[2]);
            b[3] += t;
            for (j = 0; j < 8; j++) a[(j + 1) % 8] = b[j];
            if (i % 16 == 15)
                for (j = 0; j < 16; j++)
                    w[j] += w[(j + 9) % 16] + sigma0(w[(j + 1) % 16]) + sigma1(w[(j + 14) % 16]);
        }

        for (i = 0; i < 8; i++) { a[i] += z[i]; z[i] = a[i]; }

        m += 128;
        n -= 128;
    }

    for (i = 0; i < 8; i++) ts64(x + 8 * i, z[i]);

    return n;
}

static const uint8_t iv[64] = {
    0x6a,0x09,0xe6,0x67,0xf3,0xbc,0xc9,0x08, 0xbb,0x67,0xae,0x85,0x84,0xca,0xa7,0x3b,
    0x3c,0x6e,0xf3,0x72,0xfe,0x94,0xf8,0x2b, 0xa5,0x4f,0xf5,0x3a,0x5f,0x1d,0x36,0xf1,
    0x51,0x0e,0x52,0x7f,0xad,0xe6,0x82,0xd1, 0x9b,0x05,0x68,0x8c,0x2b,0x3e,0x6c,0x1f,
    0x1f,0x83,0xd9,0xab,0xfb,0x41,0xbd,0x6b, 0x5b,0xe0,0xcd,0x19,0x13,0x7e,0x21,0x79
};

void sha512(const uint8_t *m, uint64_t n, uint8_t out[SHA512_DIGEST_LEN]) {
    uint8_t h[64], x[256];
    uint64_t i, b = n;

    for (i = 0; i < 64; i++) h[i] = iv[i];

    sha512_blocks(h, m, n);
    m += n;
    n &= 127;
    m -= n;

    for (i = 0; i < 256; i++) x[i] = 0;
    for (i = 0; i < n; i++) x[i] = m[i];
    x[n] = 128;

    n = 256 - 128 * (n < 112);
    x[n - 9] = (uint8_t)(b >> 61);
    ts64(x + n - 8, b << 3);
    sha512_blocks(h, x, n);

    for (i = 0; i < 64; i++) out[i] = h[i];
}