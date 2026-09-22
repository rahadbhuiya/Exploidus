#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sha256.h"

static void bin2hex(const uint8_t *in, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + i * 2, "%02x", in[i]);
    out[n * 2] = 0;
}

static int check(const char *name, const char *msg, const char *expected_hex) {
    uint8_t hash[32];
    char got_hex[65];
    sha256((const uint8_t *)msg, strlen(msg), hash);
    bin2hex(hash, 32, got_hex);
    int ok = strcmp(got_hex, expected_hex) == 0;
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) { printf("  got:  %s\n  want: %s\n", got_hex, expected_hex); }
    return ok;
}

/* streaming API check: same "abc" vector fed in three separate
 * sha256_update() calls instead of one, to exercise multi-call
 * accumulation (important since RFC 8731's KEX hash is computed
 * incrementally over several fields, not one buffer). */
static int check_streaming(void) {
    sha256_ctx_t ctx;
    uint8_t hash[32];
    char got_hex[65];
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)"a", 1);
    sha256_update(&ctx, (const uint8_t *)"b", 1);
    sha256_update(&ctx, (const uint8_t *)"c", 1);
    sha256_final(&ctx, hash);
    bin2hex(hash, 32, got_hex);
    const char *want = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    int ok = strcmp(got_hex, want) == 0;
    printf("sha256 streaming (a+b+c separately): %s\n", ok ? "PASS" : "FAIL");
    if (!ok) printf("  got:  %s\n  want: %s\n", got_hex, want);
    return ok;
}

int main(void) {
    int all_ok = 1;

    all_ok &= check("sha256(\"\") FIPS 180-4",
        "",
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    all_ok &= check("sha256(\"abc\") FIPS 180-4",
        "abc",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    all_ok &= check("sha256(two-block msg) FIPS 180-4",
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    all_ok &= check_streaming();

    printf(all_ok ? "\nALL FIPS 180-4 TEST VECTORS PASSED\n" : "\nFAILURE -- DO NOT USE\n");
    return all_ok ? 0 : 1;
}