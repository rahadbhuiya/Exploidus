#pragma once
#include <stdint.h>
#include <stddef.h>

#define CHACHA20POLY1305_KEYLEN  32
#define CHACHA20POLY1305_NONCELEN 12
#define CHACHA20POLY1305_TAGLEN  16

/*
 * AEAD_CHACHA20_POLY1305 (RFC 8439 Section 2.8). Encrypts `plaintext`
 * (in place is fine -- `out` may equal `plaintext`), producing
 * `ciphertext_len` bytes of ciphertext at `out` and a 16-byte tag at
 * `tag`. `aad` is authenticated but not encrypted (may be NULL if
 * aad_len is 0).
 *
 * Same key+nonce pair MUST NEVER be reused for a second message --
 * that breaks both confidentiality (keystream reuse) and integrity
 * (Poly1305 key reuse) at once. See poly1305.h.
 */
void chacha20poly1305_encrypt(uint8_t *out, const uint8_t *plaintext,
    size_t plaintext_len, const uint8_t *aad, size_t aad_len,
    const uint8_t key[CHACHA20POLY1305_KEYLEN],
    const uint8_t nonce[CHACHA20POLY1305_NONCELEN],
    uint8_t tag[CHACHA20POLY1305_TAGLEN]);

/*
 * Decrypts `ciphertext` into `out` (may alias) and verifies `tag`
 * against the AAD/ciphertext using a constant-time comparison.
 * Returns 1 if the tag is valid (plaintext in `out` is genuine), 0 if
 * it does not match -- callers MUST discard `out`'s contents on a 0
 * return rather than using unauthenticated plaintext (RFC 8439
 * Section 2.8: "The calculated tag is bitwise compared to the
 * received tag. The message is authenticated if and only if the tags
 * match.").
 */
int chacha20poly1305_decrypt(uint8_t *out, const uint8_t *ciphertext,
    size_t ciphertext_len, const uint8_t *aad, size_t aad_len,
    const uint8_t key[CHACHA20POLY1305_KEYLEN],
    const uint8_t nonce[CHACHA20POLY1305_NONCELEN],
    const uint8_t tag[CHACHA20POLY1305_TAGLEN]);