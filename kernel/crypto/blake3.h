#pragma once
#include <stdint.h>

/*
 * blake3_hash — compute a 32-byte BLAKE3 digest in one call.
 * input: data pointer and length
 * output: 32-byte digest buffer
 */
void blake3_hash(const uint8_t *input, uint64_t input_len, uint8_t *output);

/*
 * Incremental BLAKE3 API — feed data in as many calls as needed (e.g.
 * while streaming a network download to disk), without ever needing
 * to hold or re-read the whole input at once.
 *
 *   blake3_ctx_t ctx;
 *   blake3_init(&ctx);
 *   blake3_update(&ctx, chunk1, chunk1_len);
 *   blake3_update(&ctx, chunk2, chunk2_len);
 *   ...
 *   blake3_final(&ctx, digest_out);   // digest_out must hold 32 bytes
 *
 * Same single-chunk limitation as blake3_hash (see blake3.c) — this is
 * a streaming API for *delivery*, not a multi-chunk/tree implementation.
 */
typedef struct {
    uint32_t chaining[8];
    uint8_t  pending[64];   /* the most recent not-yet-compressed block */
    uint32_t pending_len;   /* bytes currently buffered in `pending` */
    int      started;       /* whether any block has been compressed yet */
} blake3_ctx_t;

void blake3_init(blake3_ctx_t *ctx);
void blake3_update(blake3_ctx_t *ctx, const uint8_t *data, uint64_t len);
void blake3_final(blake3_ctx_t *ctx, uint8_t *output);