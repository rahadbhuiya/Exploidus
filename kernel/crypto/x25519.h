#pragma once
#include <stdint.h>

/*
 * x25519_scalarmult -- X25519 function (RFC 7748 Section 5): computes
 * the Diffie-Hellman scalar multiplication out = scalar * point,
 * where "point" and "out" are Curve25519 u-coordinates.
 * scalar : 32-byte private scalar (NOT pre-clamped -- this function
 */
void x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32],
                        const uint8_t point[32]);

/* RFC 7748's fixed base point (u = 9), for deriving a public key from
 * a private scalar: x25519_scalarmult(pubkey, privkey, X25519_BASEPOINT). */
extern const uint8_t X25519_BASEPOINT[32];