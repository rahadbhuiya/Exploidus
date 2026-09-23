#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sha512.h"

static void bin2hex(const uint8_t *in, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + i * 2, "%02x", in[i]);
    out[n * 2] = 0;
}

static int check(const char *name, const char *msg, const char *expected_hex) {
    uint8_t hash[64];
    char got_hex[129];
    sha512((const uint8_t *)msg, strlen(msg), hash);
    bin2hex(hash, 64, got_hex);
    int ok = strcmp(got_hex, expected_hex) == 0;
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) { printf("  got:  %s\n  want: %s\n", got_hex, expected_hex); }
    return ok;
}

int main(void) {
    int all_ok = 1;

    all_ok &= check("sha512(\"\") FIPS 180-4", "",
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");

    all_ok &= check("sha512(\"abc\") FIPS 180-4", "abc",
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");

    all_ok &= check("sha512(two-block msg)",
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");

    printf(all_ok ? "\nALL FIPS 180-4 SHA-512 TEST VECTORS PASSED\n" : "\nFAILURE -- DO NOT USE\n");
    return all_ok ? 0 : 1;
}