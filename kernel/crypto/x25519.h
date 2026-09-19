#pragma once
#include <stdint.h>

/*
 * x25519_scalarmult -- X25519 function (RFC 7748 Section 5): computes
 * the Diffie-Hellman scalar multiplication out = scalar * point,
 * where "point" and "out" are Curve25519 u-coordinates.
 *
 * scalar : 32-byte private scalar (NOT pre-clamped -- this function
 *          applies RFC 7748's clamping internally, same as the
 *          reference implementation: scalar[0] &= 248,
 *          scalar[31] = (scalar[31] & 127) | 64)
 * point  : 32-byte u-coordinate. Use X25519_BASEPOINT to compute a
 *          public key from a private scalar; use a peer's received
 *          public key to compute the shared secret.
 * out    : 32-byte result (u-coordinate). Safe to alias `point` or
 *          reuse buffers across calls; not safe to alias `scalar`
 *          with `out` on all backends -- keep them distinct.
 *
 * This does not allocate, does not touch global state, and has no
 * failure path (RFC 7748 X25519 is defined for every input, including
 * low-order points -- callers that care about contributory behaviour
 * must reject an all-zero `out` themselves, same as every other X25519
 * caller has to).
 */
void x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32],
                        const uint8_t point[32]);

/* RFC 7748's fixed base point (u = 9), for deriving a public key from
 * a private scalar: x25519_scalarmult(pubkey, privkey, X25519_BASEPOINT). */
extern const uint8_t X25519_BASEPOINT[32];