#pragma once
#include <stdint.h>

/*
 * ChaCha20 (RFC 8439 Section 2.3/2.4) -- 32-bit counter, 96-bit nonce
 * layout (NOT D.J. Bernstein's original 64-bit/64-bit split, which
 * chacha.h in OpenSSH's tree uses for its own separate older cipher
 * suite -- this is the RFC 8439 variant, since that's what SSH's
 */
void chacha20_block(uint8_t out[64], const uint8_t key[32],
                     uint32_t counter, const uint8_t nonce[12]);

void chacha20_xor(uint8_t *out, const uint8_t *in, uint64_t len,
                   const uint8_t key[32], uint32_t counter,
                   const uint8_t nonce[12]);