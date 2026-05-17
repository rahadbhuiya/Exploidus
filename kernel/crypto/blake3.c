#include "blake3.h"
#include <string.h>
#include <stdbool.h>

/*
 * Minimal BLAKE3 implementation.
 * Supports inputs up to one chunk (1024 bytes), which is sufficient
 * for capability token authentication (44-byte input).
 *
 * Reference: https://github.com/BLAKE3-team/BLAKE3-specs/blob/master/blake3.pdf
 */

#define BLOCK_LEN   64
#define CHUNK_LEN   1024
#define OUT_LEN     32
#define KEY_LEN     32

#define CHUNK_START  (1 << 0)
#define CHUNK_END    (1 << 1)
#define ROOT         (1 << 3)

static const uint32_t IV[8] = {
    0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
    0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19
};

static const uint8_t MSG_PERMUTATION[16] = {
    2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8
};

static inline uint32_t rotr32(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void g_function(uint32_t *state, int a, int b, int c, int d,
                        uint32_t mx, uint32_t my)
{
    state[a] = state[a] + state[b] + mx;
    state[d] = rotr32(state[d] ^ state[a], 16);
    state[c] = state[c] + state[d];
    state[b] = rotr32(state[b] ^ state[c], 12);
    state[a] = state[a] + state[b] + my;
    state[d] = rotr32(state[d] ^ state[a],  8);
    state[c] = state[c] + state[d];
    state[b] = rotr32(state[b] ^ state[c],  7);
}

static void round_fn(uint32_t *state, const uint32_t *m)
{
    g_function(state, 0, 4,  8, 12, m[0],  m[1]);
    g_function(state, 1, 5,  9, 13, m[2],  m[3]);
    g_function(state, 2, 6, 10, 14, m[4],  m[5]);
    g_function(state, 3, 7, 11, 15, m[6],  m[7]);
    g_function(state, 0, 5, 10, 15, m[8],  m[9]);
    g_function(state, 1, 6, 11, 12, m[10], m[11]);
    g_function(state, 2, 7,  8, 13, m[12], m[13]);
    g_function(state, 3, 4,  9, 14, m[14], m[15]);
}

static void permute(uint32_t *m)
{
    uint32_t tmp[16];
    for (int i = 0; i < 16; i++)
        tmp[i] = m[MSG_PERMUTATION[i]];
    for (int i = 0; i < 16; i++)
        m[i] = tmp[i];
}

static void compress(const uint32_t *chaining_value,
                     const uint32_t *block_words,
                     uint64_t        counter,
                     uint32_t        block_len,
                     uint32_t        flags,
                     uint32_t       *out)
{
    uint32_t state[16];
    uint32_t block[16];

    for (int i = 0; i < 8; i++) state[i]     = chaining_value[i];
    for (int i = 0; i < 4; i++) state[8 + i] = IV[i];
    state[12] = (uint32_t)counter;
    state[13] = (uint32_t)(counter >> 32);
    state[14] = block_len;
    state[15] = flags;

    for (int i = 0; i < 16; i++) block[i] = block_words[i];

    for (int r = 0; r < 7; r++) {
        round_fn(state, block);
        permute(block);
    }

    for (int i = 0; i < 8; i++) {
        out[i]     = state[i]     ^ state[i + 8];
        out[i + 8] = state[i + 8] ^ chaining_value[i];
    }
}

/*
 * blake3_hash — single-chunk BLAKE3.
 * Handles inputs up to CHUNK_LEN (1024) bytes.
 * For the capability system this is always 44 bytes so this is sufficient.
 */
void blake3_hash(const uint8_t *input, uint64_t input_len, uint8_t *output)
{
    uint32_t chaining[8];
    uint32_t out16[16];
    uint8_t  padded_block[BLOCK_LEN];
    uint32_t block_words[16];

    for (int i = 0; i < 8; i++) chaining[i] = IV[i];

    uint64_t offset    = 0;
    bool     first     = true;

    while (offset < input_len || first) {
        first = false;

        uint64_t remaining  = input_len - offset;
        uint32_t this_block = (uint32_t)(remaining > BLOCK_LEN ? BLOCK_LEN : remaining);
        bool     is_last    = (offset + this_block >= input_len);

        memset(padded_block, 0, BLOCK_LEN);
        memcpy(padded_block, input + offset, this_block);

        /* Load block as little-endian 32-bit words */
        for (int i = 0; i < 16; i++) {
            block_words[i] =
                ((uint32_t)padded_block[i*4 + 0])        |
                ((uint32_t)padded_block[i*4 + 1] <<  8)  |
                ((uint32_t)padded_block[i*4 + 2] << 16)  |
                ((uint32_t)padded_block[i*4 + 3] << 24);
        }

        uint32_t flags = 0;
        if (offset == 0)    flags |= CHUNK_START;
        if (is_last)        flags |= CHUNK_END | ROOT;

        compress(chaining, block_words, 0, this_block, flags, out16);

        if (is_last) break;

        /* Update chaining value for next block */
        for (int i = 0; i < 8; i++) chaining[i] = out16[i];
        offset += this_block;
    }

    /* Write output as little-endian bytes */
    for (int i = 0; i < 8; i++) {
        output[i*4 + 0] = (uint8_t)(out16[i]);
        output[i*4 + 1] = (uint8_t)(out16[i] >>  8);
        output[i*4 + 2] = (uint8_t)(out16[i] >> 16);
        output[i*4 + 3] = (uint8_t)(out16[i] >> 24);
    }
}
