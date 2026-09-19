#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "x25519.h"

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

static int check(const char *name, const char *scalar_hex,
                  const char *u_hex, const char *expected_hex) {
    uint8_t scalar[32], u[32], out[32];
    char got_hex[65];

    hex2bin(scalar_hex, scalar, 32);
    hex2bin(u_hex, u, 32);

    x25519_scalarmult(out, scalar, u);
    bin2hex(out, 32, got_hex);

    int ok = (strcmp(got_hex, expected_hex) == 0);
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        printf("  expected: %s\n", expected_hex);
        printf("  got:      %s\n", got_hex);
    }
    return ok;
}

int main(void) {
    int all_ok = 1;

    /* RFC 7748 Section 5.2, X25519 test vector 1 (official) */
    all_ok &= check("RFC7748 5.2 vector 1",
        "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
        "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
        "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");

    /* RFC 7748 Section 5.2, X25519 test vector 2 (official) */
    all_ok &= check("RFC7748 5.2 vector 2",
        "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
        "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
        "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");

    printf(all_ok ? "\nBOTH RFC 7748 TEST VECTORS PASSED\n" : "\nFAILURE -- DO NOT USE\n");
    return all_ok ? 0 : 1;
}