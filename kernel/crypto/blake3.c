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
 * blake3_hash — single-shot convenience wrapper over the incremental API.
 */
void blake3_hash(const uint8_t *input, uint64_t input_len, uint8_t *output)
{
    blake3_ctx_t ctx;
    blake3_init(&ctx);
    blake3_update(&ctx, input, input_len);
    blake3_final(&ctx, output);
}

/*
 * Incremental BLAKE3.
 *
 * The original (and any correct BLAKE3 implementation) must compress
 * the LAST block with CHUNK_END|ROOT flags, but a streaming caller
 * doesn't know which block is last until it stops feeding data. So
 * this implementation always holds back the most recently completed
 * 64-byte block ("pending") uncompressed: a block is only compressed
 * once we *know* more data follows it (i.e. once a new block starts
 * filling up), or once blake3_final() is called and we know for
 * certain it was the last one.
 */
void blake3_init(blake3_ctx_t *ctx)
{
    for (int i = 0; i < 8; i++) ctx->chaining[i] = IV[i];
    ctx->pending_len = 0;
    ctx->started     = 0;
}

/* Compress ctx->pending (ctx->pending_len bytes, zero-padded) as a
 * NON-final block and fold the result into ctx->chaining. Only ever
 * called from blake3_update for a block we already know isn't last
 * (more data has arrived after it) — the actual last block is handled
 * separately, inline, in blake3_final. */
static void blake3_compress_intermediate(blake3_ctx_t *ctx)
{
    uint8_t  padded_block[BLOCK_LEN];
    uint32_t block_words[16];
    uint32_t out16[16];

    memset(padded_block, 0, BLOCK_LEN);
    memcpy(padded_block, ctx->pending, ctx->pending_len);

    for (int i = 0; i < 16; i++) {
        block_words[i] =
            ((uint32_t)padded_block[i*4 + 0])        |
            ((uint32_t)padded_block[i*4 + 1] <<  8)  |
            ((uint32_t)padded_block[i*4 + 2] << 16)  |
            ((uint32_t)padded_block[i*4 + 3] << 24);
    }

    uint32_t flags = ctx->started ? 0 : CHUNK_START;

    compress(ctx->chaining, block_words, 0, ctx->pending_len, flags, out16);

    for (int i = 0; i < 8; i++) ctx->chaining[i] = out16[i];
    ctx->started = 1;
}

void blake3_update(blake3_ctx_t *ctx, const uint8_t *data, uint64_t len)
{
    while (len > 0) {
        if (ctx->pending_len == BLOCK_LEN) {
            /* pending block is full and more data is arriving, so it's
             * definitely not the last block — compress it now. */
            blake3_compress_intermediate(ctx);
            ctx->pending_len = 0;
        }

        uint32_t space = BLOCK_LEN - ctx->pending_len;
        uint64_t take  = (len < space) ? len : space;

        memcpy(ctx->pending + ctx->pending_len, data, take);
        ctx->pending_len += (uint32_t)take;
        data += take;
        len  -= take;
    }
}

void blake3_final(blake3_ctx_t *ctx, uint8_t *output)
{
    /* Whatever is left in `pending` (0..64 bytes, possibly 0 for an
     * empty input) is the true last block — compress it for real here
     * so we can grab out16 directly. */
    uint8_t  padded_block[BLOCK_LEN];
    uint32_t block_words[16];
    uint32_t out16[16];

    memset(padded_block, 0, BLOCK_LEN);
    memcpy(padded_block, ctx->pending, ctx->pending_len);

    for (int i = 0; i < 16; i++) {
        block_words[i] =
            ((uint32_t)padded_block[i*4 + 0])        |
            ((uint32_t)padded_block[i*4 + 1] <<  8)  |
            ((uint32_t)padded_block[i*4 + 2] << 16)  |
            ((uint32_t)padded_block[i*4 + 3] << 24);
    }

    uint32_t flags = CHUNK_END | ROOT;
    if (!ctx->started) flags |= CHUNK_START;

    compress(ctx->chaining, block_words, 0, ctx->pending_len, flags, out16);

    for (int i = 0; i < 8; i++) {
        output[i*4 + 0] = (uint8_t)(out16[i]);
        output[i*4 + 1] = (uint8_t)(out16[i] >>  8);
        output[i*4 + 2] = (uint8_t)(out16[i] >> 16);
        output[i*4 + 3] = (uint8_t)(out16[i] >> 24);
    }
}