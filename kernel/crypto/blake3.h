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
 * blake3_ctx_t ctx;
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