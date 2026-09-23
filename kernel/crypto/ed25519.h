#pragma once
#include <stdint.h>
#include <stddef.h>

#define ED25519_SEED_LEN       32
#define ED25519_PUBLIC_KEY_LEN 32
#define ED25519_SECRET_KEY_LEN 64  /* seed(32) || public_key(32) --
                                     * same layout TweetNaCl/NaCl use,
                                     * NOT the same as some other
                                     * libraries that store seed alone */
#define ED25519_SIGNATURE_LEN  64

/*
 * ed25519_keypair_from_seed -- derives a public/secret key pair from
 * a 32-byte seed (RFC 8032 calls this the "private key"; the derived
 * 64-byte scalar-and-prefix pair below is what actually signs).
 * Deterministic: the same seed always produces the same key pair --
 * callers are responsible for generating a genuinely random,
 * never-reused seed themselves (this file has no RNG dependency by
 * design, to stay portable/host-testable).
 *
 * For sshd's host key: generate a seed once (e.g. from the kernel's
 * existing RDRAND-backed capability-token entropy), derive the key
 * pair, and persist ONLY the seed (or the resulting secret_key) --
 * losing it means generating and distributing a new host key.
 */
void ed25519_keypair_from_seed(uint8_t public_key[ED25519_PUBLIC_KEY_LEN],
                                uint8_t secret_key[ED25519_SECRET_KEY_LEN],
                                const uint8_t seed[ED25519_SEED_LEN]);

/* Signs `message` (message_len bytes) with secret_key, writing a
 * detached 64-byte signature to `signature`. */
void ed25519_sign(uint8_t signature[ED25519_SIGNATURE_LEN],
                   const uint8_t *message, size_t message_len,
                   const uint8_t secret_key[ED25519_SECRET_KEY_LEN]);

/* Verifies `signature` over `message` against public_key. Returns 1
 * if valid, 0 if not -- callers MUST treat 0 as "reject", same as
 * chacha20poly1305_decrypt()'s tag-check convention elsewhere in this
 * crypto/ directory. */
int ed25519_verify(const uint8_t signature[ED25519_SIGNATURE_LEN],
                    const uint8_t *message, size_t message_len,
                    const uint8_t public_key[ED25519_PUBLIC_KEY_LEN]);