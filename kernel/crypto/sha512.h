#pragma once
#include <stdint.h>
#include <stddef.h>

#define SHA512_DIGEST_LEN 64

/* One-shot SHA-512 (FIPS 180-4). Needed as Ed25519's internal hash
 * function (RFC 8032 uses SHA-512 throughout, not SHA-256 -- SHA-256
 * is what sha256.c is for, RFC 8731's curve25519-sha256 KEX hash). */
void sha512(const uint8_t *m, uint64_t n, uint8_t out[SHA512_DIGEST_LEN]);