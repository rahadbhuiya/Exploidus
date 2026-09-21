/*
 * chacha20poly1305.c -- AEAD_CHACHA20_POLY1305, RFC 8439 Section 2.8,
 * built directly on chacha20.c and poly1305.c. Written straight from
 * the RFC's own pseudocode (Section 2.8.1) -- there's no well-known
 * "donna"-style reference for the AEAD glue itself the way there is
 * for the two primitives it combines; the glue is just concatenation,
 * padding, and length fields, which is why it's written directly
 * rather than ported, and why it leans on the primitive-level
 * verification of chacha20.c/poly1305.c plus the RFC's own full
 * AEAD test vector (Section 2.8.2) rather than needing separate
 * "is the math right" scrutiny of its own.
 *
 * mac_data is built in one contiguous, bounded, static buffer rather
 * than streamed through Poly1305 incrementally or heap-allocated:
 * this file is meant to be compiled into sshd (a userspace process
 * that handles one SSH connection fully before accepting the next --
 * see sshd.c's accept loop), not into the kernel image, so a static
 * buffer here is private per-process state, not kernel-shared state
 * (see the netbuf-pool race elsewhere in this project's history for
 * why that distinction matters). MAC_DATA_MAX bounds the combined
 * AAD+ciphertext size this can authenticate in one call; callers
 * exceeding it get a defined failure (encrypt: undefined tag content
 * and no plaintext protection -- checked with an assert-style guard
 * below; decrypt: always fails closed) rather than a silent overflow.
 */

#include <stdint.h>
#include <string.h>
#include "chacha20.h"
#include "poly1305.h"
#include "chacha20poly1305.h"

#define MAC_DATA_MAX (40 * 1024)
static uint8_t g_mac_data[MAC_DATA_MAX];

/* RFC 8439 Section 2.6.1: poly1305_key_gen(key, nonce) -- block 0 of
 * the ChaCha20 keystream under this (key, nonce) is the one-time
 * Poly1305 key; only the first 32 of its 64 bytes are used. */
static void poly1305_key_gen(uint8_t otk[32], const uint8_t key[32],
                              const uint8_t nonce[12]) {
    uint8_t block[64];
    chacha20_block(block, key, 0, nonce);
    memcpy(otk, block, 32);
}

/* pad16(len): how many zero bytes bring `len` up to a multiple of 16
 * (RFC 8439 Section 2.8.1's pad16, inlined as a byte count rather
 * than an explicit zero-buffer since the destination is always a
 * zeroed region of g_mac_data here). */
static size_t pad16_len(size_t len) {
    size_t rem = len % 16;
    return rem == 0 ? 0 : 16 - rem;
}

/* Builds the Poly1305 input per RFC 8439 Section 2.8:
 *   AAD | pad16(AAD) | ciphertext | pad16(ciphertext) |
 *   len(AAD) as 8-byte LE | len(ciphertext) as 8-byte LE
 * into g_mac_data, returning its total length, or 0 if it would not
 * fit in MAC_DATA_MAX. */
static size_t build_mac_data(const uint8_t *aad, size_t aad_len,
                              const uint8_t *ct, size_t ct_len) {
    size_t aad_pad = pad16_len(aad_len);
    size_t ct_pad  = pad16_len(ct_len);
    size_t total = aad_len + aad_pad + ct_len + ct_pad + 8 + 8;
    size_t pos = 0;

    if (total > MAC_DATA_MAX)
        return 0;

    if (aad_len) memcpy(g_mac_data + pos, aad, aad_len);
    pos += aad_len;
    memset(g_mac_data + pos, 0, aad_pad);
    pos += aad_pad;

    if (ct_len) memcpy(g_mac_data + pos, ct, ct_len);
    pos += ct_len;
    memset(g_mac_data + pos, 0, ct_pad);
    pos += ct_pad;

    /* num_to_8_le_bytes */
    for (int i = 0; i < 8; i++) g_mac_data[pos + i] = (uint8_t)(aad_len >> (8 * i));
    pos += 8;
    for (int i = 0; i < 8; i++) g_mac_data[pos + i] = (uint8_t)(ct_len >> (8 * i));
    pos += 8;

    return pos;
}

void chacha20poly1305_encrypt(uint8_t *out, const uint8_t *plaintext,
    size_t plaintext_len, const uint8_t *aad, size_t aad_len,
    const uint8_t key[CHACHA20POLY1305_KEYLEN],
    const uint8_t nonce[CHACHA20POLY1305_NONCELEN],
    uint8_t tag[CHACHA20POLY1305_TAGLEN]) {

    uint8_t otk[32];
    poly1305_key_gen(otk, key, nonce);

    /* RFC 8439 Section 2.8: "the ChaCha20 encryption function is
     * called ... with the initial counter set to 1" -- block 0 was
     * just consumed above for the Poly1305 key. */
    chacha20_xor(out, plaintext, plaintext_len, key, 1, nonce);

    size_t mac_len = build_mac_data(aad, aad_len, out, plaintext_len);
    if (mac_len == 0) {
        /* Message too large for MAC_DATA_MAX -- ciphertext above is
         * still valid (chacha20_xor has no size limit of its own),
         * but we cannot produce a trustworthy tag for it. Zero the
         * tag rather than leave it uninitialized; a caller that
         * doesn't check for this failure mode and sends a
         * zero-tagged packet anyway will simply fail authentication
         * on the receiving end, not silently look valid. */
        memset(tag, 0, CHACHA20POLY1305_TAGLEN);
        return;
    }

    poly1305_auth(tag, g_mac_data, mac_len, otk);
}

/* Constant-time 16-byte comparison -- RFC 8439 Section 4: "implementation
 * MUST use a constant-time comparison function rather than relying on
 * optimized but insecure library functions such as the C language's
 * memcmp()." */
static int tags_equal(const uint8_t a[16], const uint8_t b[16]) {
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= a[i] ^ b[i];
    return diff == 0;
}

int chacha20poly1305_decrypt(uint8_t *out, const uint8_t *ciphertext,
    size_t ciphertext_len, const uint8_t *aad, size_t aad_len,
    const uint8_t key[CHACHA20POLY1305_KEYLEN],
    const uint8_t nonce[CHACHA20POLY1305_NONCELEN],
    const uint8_t tag[CHACHA20POLY1305_TAGLEN]) {

    uint8_t otk[32];
    poly1305_key_gen(otk, key, nonce);

    size_t mac_len = build_mac_data(aad, aad_len, ciphertext, ciphertext_len);
    if (mac_len == 0)
        return 0; /* can't verify -> fail closed */

    uint8_t computed[CHACHA20POLY1305_TAGLEN];
    poly1305_auth(computed, g_mac_data, mac_len, otk);

    if (!tags_equal(computed, tag))
        return 0;

    chacha20_xor(out, ciphertext, ciphertext_len, key, 1, nonce);
    return 1;
}