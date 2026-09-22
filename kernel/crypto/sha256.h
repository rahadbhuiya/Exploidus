#pragma once
#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_LEN 32
#define SHA256_BLOCK_LEN  64

typedef struct {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} sha256_ctx_t;

/*
 * Standard init/update/final streaming API -- needed for RFC 8731's
 * curve25519-sha256 SSH key-exchange hash, which is computed
 * incrementally over several concatenated fields (client ID string,
 * server ID string, KEXINIT payloads, host key, ephemeral public
 * keys, shared secret) as the handshake progresses, not over one
 * buffer available all at once.
 */
void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t hash[SHA256_DIGEST_LEN]);

/* Convenience one-shot wrapper for when the whole message is already
 * in one buffer. */
void sha256(const uint8_t *data, size_t len, uint8_t hash[SHA256_DIGEST_LEN]);