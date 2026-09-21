#pragma once
#include <stdint.h>
#include <stddef.h>

#define POLY1305_KEYLEN 32
#define POLY1305_TAGLEN 16

/*
 * poly1305_auth -- RFC 8439 Section 2.5 one-time message
 * authentication code. `key` is a 32-byte ONE-TIME key (r || s) --
 * NEVER reuse a Poly1305 key across two different messages; see
 * poly1305_key_gen() in chacha20poly1305.c for how the AEAD
 * construction derives a fresh one per message from the session key
 * and nonce.
 */
void poly1305_auth(uint8_t out[POLY1305_TAGLEN], const uint8_t *m,
                    size_t inlen, const uint8_t key[POLY1305_KEYLEN]);