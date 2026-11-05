/*
 * chacha20.c -- ChaCha20 stream cipher, RFC 8439 Section 2.3/2.4
 * layout (32-bit block counter, 96-bit nonce).
 * The quarter-round / double-round core (QUARTERROUND macro, the
 * constant "expand 32-byte k", the add/rotate/xor sequence) is
 */

#include <stdint.h>
#include <string.h>
#include "chacha20.h"

typedef uint8_t u8;
typedef uint32_t u32;

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

#define U8TO32_LITTLE(p) \
    (((u32)((p)[0])      ) | \
     ((u32)((p)[1]) <<  8) | \
     ((u32)((p)[2]) << 16) | \
     ((u32)((p)[3]) << 24))

#define U32TO8_LITTLE(p, v) \
    do { \
        (p)[0] = (u8)((v)      ); \
        (p)[1] = (u8)((v) >>  8); \
        (p)[2] = (u8)((v) >> 16); \
        (p)[3] = (u8)((v) >> 24); \
    } while (0)

#define QUARTERROUND(a,b,c,d) \
    a += b; d ^= a; d = ROTL32(d,16); \
    c += d; b ^= c; b = ROTL32(b,12); \
    a += b; d ^= a; d = ROTL32(d, 8); \
    c += d; b ^= c; b = ROTL32(b, 7);

static const u8 sigma[16] = { 'e','x','p','a','n','d',' ','3','2','-','b','y','t','e',' ','k' };

void chacha20_block(uint8_t out[64], const uint8_t key[32],
                     uint32_t counter, const uint8_t nonce[12]) {
    u32 x[16], s[16];
    int i;

    s[0]  = U8TO32_LITTLE((const u8 *)sigma + 0);
    s[1]  = U8TO32_LITTLE((const u8 *)sigma + 4);
    s[2]  = U8TO32_LITTLE((const u8 *)sigma + 8);
    s[3]  = U8TO32_LITTLE((const u8 *)sigma + 12);
    s[4]  = U8TO32_LITTLE(key + 0);
    s[5]  = U8TO32_LITTLE(key + 4);
    s[6]  = U8TO32_LITTLE(key + 8);
    s[7]  = U8TO32_LITTLE(key + 12);
    s[8]  = U8TO32_LITTLE(key + 16);
    s[9]  = U8TO32_LITTLE(key + 20);
    s[10] = U8TO32_LITTLE(key + 24);
    s[11] = U8TO32_LITTLE(key + 28);
    s[12] = counter;
    s[13] = U8TO32_LITTLE(nonce + 0);
    s[14] = U8TO32_LITTLE(nonce + 4);
    s[15] = U8TO32_LITTLE(nonce + 8);

    memcpy(x, s, sizeof(x));

    for (i = 0; i < 10; i++) {
        /* column round */
        QUARTERROUND(x[0], x[4], x[8],  x[12]);
        QUARTERROUND(x[1], x[5], x[9],  x[13]);
        QUARTERROUND(x[2], x[6], x[10], x[14]);
        QUARTERROUND(x[3], x[7], x[11], x[15]);
        /* diagonal round */
        QUARTERROUND(x[0], x[5], x[10], x[15]);
        QUARTERROUND(x[1], x[6], x[11], x[12]);
        QUARTERROUND(x[2], x[7], x[8],  x[13]);
        QUARTERROUND(x[3], x[4], x[9],  x[14]);
    }

    for (i = 0; i < 16; i++)
        x[i] += s[i];

    for (i = 0; i < 16; i++)
        U32TO8_LITTLE(out + i * 4, x[i]);
}

void chacha20_xor(uint8_t *out, const uint8_t *in, uint64_t len,
                   const uint8_t key[32], uint32_t counter,
                   const uint8_t nonce[12]) {
    uint8_t block[64];
    uint64_t off = 0;

    while (len > 0) {
        uint64_t n = len < 64 ? len : 64;
        uint64_t i;

        chacha20_block(block, key, counter, nonce);
        counter++; /* RFC 8439 Section 3, note 1: 2^32 blocks max per
                     * (key,nonce) -- callers are responsible for never
                     * encrypting enough data under one nonce to wrap
                     * this counter. */

        for (i = 0; i < n; i++)
            out[off + i] = in[off + i] ^ block[i];

        off += n;
        len -= n;
    }
}