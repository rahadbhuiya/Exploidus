#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ed25519.h"

static int g_fail = 0;

static void hex2bin(const char *hex, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        unsigned int b;
        sscanf(hex + i * 2, "%2x", &b);
        out[i] = (uint8_t)b;
    }
}

static void bin2hex(const uint8_t *in, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + i * 2, "%02x", in[i]);
    out[n * 2] = 0;
}

static void check_bytes(const char *name, const uint8_t *got, const uint8_t *want, int n) {
    if (memcmp(got, want, n) == 0) {
        printf("%s: PASS\n", name);
    } else {
        char g[512], w[512];
        bin2hex(got, n, g);
        bin2hex(want, n, w);
        printf("%s: FAIL\n  got:  %s\n  want: %s\n", name, g, w);
        g_fail = 1;
    }
}

/* Runs keypair derivation + sign + verify for one RFC 8032 test
 * vector, checking public key, signature, AND that verification
 * actually accepts the valid signature and rejects a tampered one. */
static void run_vector(const char *name, const char *seed_hex,
                        const char *pubkey_hex, const uint8_t *msg,
                        size_t msg_len, const char *sig_hex) {
    uint8_t seed[32], want_pk[32], want_sig[64];
    uint8_t pubkey[32], secret_key[64], sig[64];
    char label[128];

    hex2bin(seed_hex, seed, 32);
    hex2bin(pubkey_hex, want_pk, 32);
    hex2bin(sig_hex, want_sig, 64);

    ed25519_keypair_from_seed(pubkey, secret_key, seed);
    snprintf(label, sizeof(label), "%s pubkey", name);
    check_bytes(label, pubkey, want_pk, 32);

    ed25519_sign(sig, msg, msg_len, secret_key);
    snprintf(label, sizeof(label), "%s signature", name);
    check_bytes(label, sig, want_sig, 64);

    int ok = ed25519_verify(sig, msg, msg_len, pubkey);
    printf("%s verify (valid sig): %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) g_fail = 1;

    /* tamper check */
    uint8_t bad_sig[64];
    memcpy(bad_sig, sig, 64);
    bad_sig[0] ^= 0x01;
    int should_fail = ed25519_verify(bad_sig, msg, msg_len, pubkey);
    printf("%s verify (tampered sig): %s\n", name, should_fail ? "FAIL (accepted!)" : "PASS (rejected)");
    if (should_fail) g_fail = 1;
}

int main(void) {
    /* RFC 8032 Section 7.1, TEST 1: empty message */
    run_vector("TEST1",
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        (const uint8_t *)"", 0,
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");

    /* TEST 2: message = 0x72 */
    {
        uint8_t msg[1] = {0x72};
        run_vector("TEST2",
            "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
            "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
            msg, 1,
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    }

    /* TEST 3: message = 0xaf82 */
    {
        uint8_t msg[2] = {0xaf, 0x82};
        run_vector("TEST3",
            "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
            "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
            msg, 2,
            "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a");
    }

    printf(g_fail ? "\nFAILURE -- DO NOT USE\n" : "\nALL RFC 8032 TEST VECTORS PASSED\n");
    return g_fail ? 1 : 0;
}