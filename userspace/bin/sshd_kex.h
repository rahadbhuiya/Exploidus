#pragma once
#include <stdint.h>
#include "../libc/syscall.h"

/*
 * Server side of curve25519-sha256 key exchange (RFC 8731, which is
 * wire-compatible with OpenSSH's older curve25519-sha256@libssh.org)
 * with an ssh-ed25519 host key (RFC 8709).
 *
 *   C -> S : SSH_MSG_KEX_ECDH_INIT   (30)  string Q_C
 *   S -> C : SSH_MSG_KEX_ECDH_REPLY  (31)  string K_S, string Q_S,
 *                                          string sig(H)
 *   S -> C : SSH_MSG_NEWKEYS         (21)
 *   C -> S : SSH_MSG_NEWKEYS         (21)
 *
 * This module only does the handshake and key derivation. It does NOT
 * switch the connection to an encrypted packet layer -- the derived
 * keys are handed back in ssh_keys_t for whatever implements that.
 */

#define SSH_MSG_NEWKEYS         21
#define SSH_MSG_KEX_ECDH_INIT   30
#define SSH_MSG_KEX_ECDH_REPLY  31

typedef struct {
    uint8_t public_key[32];
    uint8_t secret_key[64];  /* seed(32) || public_key(32), see ed25519.h */
} ssh_hostkey_t;

/*
 * Keys for chacha20-poly1305@openssh.com. That cipher takes 64 bytes
 * of key per direction (OpenSSH chachapoly_new()): bytes 0..31 are
 * K_2 (the "main" key: payload + Poly1305 key), bytes 32..63 are K_1
 * (the "header" key: encrypts the 4-byte packet length). Note this is
 * NOT the RFC 8439 AEAD in chacha20poly1305.c -- see sshd_kex.c.
 */
typedef struct {
    uint8_t session_id[32];     /* = exchange hash H of the first KEX */
    uint8_t key_c2s[64];        /* RFC 4253 7.2 letter 'C' */
    uint8_t key_s2c[64];        /* RFC 4253 7.2 letter 'D' */
} ssh_keys_t;

/* Writes the ssh-ed25519 public key blob (RFC 8709 / RFC 4253 6.6:
 * string "ssh-ed25519", string key[32]) into out (>= 64 bytes).
 * Returns its length (51). The SHA-256 of this blob, base64'd, is the
 * fingerprint `ssh` prints on first connect. */
uint32_t ssh_hostkey_blob(uint8_t *out, const uint8_t public_key[32]);

/*
 * Runs the server side of the handshake above on `fd`, right after
 * both KEXINITs have been exchanged.
 *
 *   v_c / v_s : identification strings, WITHOUT the trailing CR LF
 *   i_c / i_s : the raw KEXINIT payloads (starting at the type byte)
 *
 * Returns 1 on success (both NEWKEYS exchanged, *out filled in), 0 on
 * any failure (caller must close the connection; *out is zeroed).
 */
int ssh_kex_ecdh_server(cap_token_t cap, int fd,
                        const uint8_t *v_c, uint32_t v_c_len,
                        const uint8_t *v_s, uint32_t v_s_len,
                        const uint8_t *i_c, uint32_t i_c_len,
                        const uint8_t *i_s, uint32_t i_s_len,
                        const ssh_hostkey_t *hk, ssh_keys_t *out);