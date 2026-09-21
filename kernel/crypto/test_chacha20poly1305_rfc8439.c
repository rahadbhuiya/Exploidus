#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "chacha20.h"
#include "poly1305.h"
#include "chacha20poly1305.h"

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

/* ---- ChaCha20 block function test vectors (RFC 8439 2.3.2 + A.1) ---- */
static void test_chacha20_block(void) {
    uint8_t out[64], key[32], nonce[12], want[64];

    /* 2.3.2: key=00..1f, nonce=(9,0x4a,0), counter=1 */
    hex2bin("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, 32);
    hex2bin("000000090000004a00000000", nonce, 12);
    chacha20_block(out, key, 1, nonce);
    hex2bin("10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4"
            "ed2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e",
            want, 64);
    check_bytes("chacha20_block RFC8439 2.3.2", out, want, 64);

    /* A.1 Test Vector #1: all-zero key/nonce, counter=0 */
    memset(key, 0, 32);
    memset(nonce, 0, 12);
    chacha20_block(out, key, 0, nonce);
    hex2bin("76b8e0ada0f13d90405d6ae55386bd28bdd219b8a08ded1aa836efcc8b770dc7"
            "da41597c5157488d7724e03fb8d84a376a43b8f41518a11cc387b669b2ee6586",
            want, 64);
    check_bytes("chacha20_block A.1 #1", out, want, 64);

    /* A.1 Test Vector #4: key[1]=0xff, nonce all-zero, counter=2 */
    memset(key, 0, 32);
    key[1] = 0xff;
    memset(nonce, 0, 12);
    chacha20_block(out, key, 2, nonce);
    hex2bin("72d54dfbf12ec44b362692df94137f328fea8da73990265ec1bbbea1ae9af0c"
            "a13b25aa26cb4a648cb9b9d1be65b2c0924a66c54d545ec1b7374f4872e99f096",
            want, 64);
    check_bytes("chacha20_block A.1 #4", out, want, 64);
}

/* ---- ChaCha20 encryption test vector (RFC 8439 2.4.2) ---- */
static void test_chacha20_encrypt(void) {
    uint8_t key[32], nonce[12], pt[114], ct[114], want[114];

    hex2bin("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, 32);
    hex2bin("000000000000004a00000000", nonce, 12);
    hex2bin("4c616469657320616e642047656e746c656d656e206f662074686520636c617"
            "373206f66202739393a204966204920636f756c64206f6666657220796f75206"
            "f6e6c79206f6e652074697020666f7220746865206675747572652c2073756e7"
            "3637265656e20776f756c642062652069742e",
            pt, 114);
    chacha20_xor(ct, pt, 114, key, 1, nonce);
    hex2bin("6e2e359a2568f98041ba0728dd0d6981e97e7aec1d43"
            "60c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b3571639d624"
            "e65152ab8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e52b"
            "c514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2785e"
            "42874d",
            want, 114);
    check_bytes("chacha20_xor RFC8439 2.4.2", ct, want, 114);
}

/* ---- Poly1305 MAC test vectors (RFC 8439 2.5.2 + A.3 edge cases) ---- */
static void test_poly1305(void) {
    uint8_t key[32], tag[16], want[16];

    /* 2.5.2 */
    hex2bin("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key, 32);
    const char *msg = "Cryptographic Forum Research Group";
    poly1305_auth(tag, (const uint8_t *)msg, strlen(msg), key);
    hex2bin("a8061dc1305136c6c22b8baf0c0127a9", want, 16);
    check_bytes("poly1305 RFC8439 2.5.2", tag, want, 16);

    /* A.3 #1: all-zero key and message -> all-zero tag */
    memset(key, 0, 32);
    uint8_t zeros[64] = {0};
    poly1305_auth(tag, zeros, 64, key);
    memset(want, 0, 16);
    check_bytes("poly1305 A.3 #1 (zero key/msg)", tag, want, 16);

    /* A.3 #5: R=2, S=0, data=16 bytes of 0xff -- 130-bit partial
     * reduction edge case */
    {
        uint8_t k[32], data[16];
        hex2bin("02000000000000000000000000000000", k, 16);
        memset(k + 16, 0, 16);
        memset(data, 0xff, 16);
        poly1305_auth(tag, data, 16, k);
        hex2bin("03000000000000000000000000000000", want, 16);
        check_bytes("poly1305 A.3 #5 (130-bit reduction)", tag, want, 16);
    }

    /* A.3 #8: final polynomial result exactly 2^130-5 */
    {
        uint8_t k[32], data[48];
        hex2bin("01000000000000000000000000000000", k, 16);
        memset(k + 16, 0, 16);
        hex2bin("ffffffffffffffffffffffffffffffff"
                "fbfefefefefefefefefefefefefefefe"
                "01010101010101010101010101010101", data, 48);
        poly1305_auth(tag, data, 48, k);
        memset(want, 0, 16);
        check_bytes("poly1305 A.3 #8 (result == 2^130-5)", tag, want, 16);
    }
}

/* ---- Full AEAD test vector (RFC 8439 2.8.2), plus round-trip and
 * tamper-detection checks against the same vector ---- */
static void test_aead(void) {
    uint8_t key[32], nonce[12], aad[12];
    uint8_t pt[114], ct[114], want_ct[114], tag[16], want_tag[16];

    hex2bin("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key, 32);
    hex2bin("070000004041424344454647", nonce, 12);
    hex2bin("50515253c0c1c2c3c4c5c6c7", aad, 12);
    hex2bin("4c616469657320616e642047656e746c656d656e206f662074686520636c617"
            "373206f66202739393a204966204920636f756c64206f6666657220796f75206"
            "f6e6c79206f6e652074697020666f7220746865206675747572652c2073756e7"
            "3637265656e20776f756c642062652069742e",
            pt, 114);

    chacha20poly1305_encrypt(ct, pt, 114, aad, 12, key, nonce, tag);

    hex2bin("d31a8d34648e60db7b86afbc53ef7ec2a4aded5"
            "1296e08fea9e2b5a736ee62d63dbea45e8ca967128"
            "2fafb69da92728b1a71de0a9e060b2905d6a5b67ecd"
            "3b3692ddbd7f2d778b8c9803aee328091b58fab324e4"
            "fad675945585808b4831d7bc3ff4def08e4b7a9de576"
            "d26586cec64b6116",
            want_ct, 114);
    check_bytes("chacha20poly1305 RFC8439 2.8.2 ciphertext", ct, want_ct, 114);

    hex2bin("1ae10b594f09e26a7e902ecbd0600691", want_tag, 16);
    check_bytes("chacha20poly1305 RFC8439 2.8.2 tag", tag, want_tag, 16);

    /* round-trip: decrypt what we just encrypted, confirm tag verifies
     * and plaintext comes back byte-for-byte */
    uint8_t decrypted[114];
    int ok = chacha20poly1305_decrypt(decrypted, ct, 114, aad, 12, key, nonce, tag);
    printf("chacha20poly1305 round-trip tag verify: %s\n", ok ? "PASS" : "FAIL");
    if (!ok) g_fail = 1;
    check_bytes("chacha20poly1305 round-trip plaintext", decrypted, pt, 114);

    /* tamper check: flip a ciphertext byte, confirm decrypt now fails */
    uint8_t tampered_ct[114];
    memcpy(tampered_ct, ct, 114);
    tampered_ct[0] ^= 0x01;
    uint8_t scratch[114];
    int should_fail = chacha20poly1305_decrypt(scratch, tampered_ct, 114, aad, 12, key, nonce, tag);
    printf("chacha20poly1305 tamper detection: %s\n",
           should_fail ? "FAIL (accepted tampered ciphertext!)" : "PASS (rejected)");
    if (should_fail) g_fail = 1;
}

int main(void) {
    test_chacha20_block();
    test_chacha20_encrypt();
    test_poly1305();
    test_aead();

    printf(g_fail ? "\nFAILURE -- DO NOT USE\n" : "\nALL RFC 8439 TEST VECTORS PASSED\n");
    return g_fail ? 1 : 0;
}