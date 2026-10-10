/* Test-only: pulls in sshd_kex.c to reach its static mpint/KDF helpers.
 * No <stdio.h> (it clashes with the shim's puts/putc) -- own hex I/O. */
#include "sshd_kex.c"
static int hv(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
static void unhex(uint8_t *o, const char *h, int n) { for (int i = 0; i < n; i++) o[i] = (uint8_t)(hv(h[2*i]) << 4 | hv(h[2*i+1])); }
static void hex(const uint8_t *p, int n) {
    static const char D[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { putc(D[p[i] >> 4]); putc(D[p[i] & 15]); }
    putc('\n');
}
int main(int argc, char **argv) {
    uint8_t k[32], H[32], mp[40], kc[64], kd[64];
    (void)argc; unhex(k, argv[1], 32); unhex(H, argv[2], 32);
    uint32_t n = mpint_from_be32(mp, k);
    hex(mp, (int)n);
    derive_key(kc, 64, mp, n, H, 'C', H);
    derive_key(kd, 64, mp, n, H, 'D', H);
    hex(kc, 64); hex(kd, 64);
    return 0;
}