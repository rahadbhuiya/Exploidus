/*
 * Exploidus minimal stdlib.c
 */
#include "stdlib.h"
#include "string.h"
#include "syscall.h"
#include "ctype.h"
#include "math.h"

void exit(int code)
{
    syscall1(SYS_EXIT, (uint64_t)(int64_t)code);
    __builtin_unreachable();
}

void abort(void)
{
    exit(134);
}

int atoi(const char *s)  { return (int)strtol(s, (char **)0, 10); }
long atol(const char *s) { return strtol(s, (char **)0, 10); }
long long atoll(const char *s) { return strtoll(s, (char **)0, 10); }

/*  qsort (insertion sort — simple, no stack overflow)  */
void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void *, const void *))
{
    uint8_t *b = base;
    uint8_t *tmp = malloc(size);
    if (!tmp) return;
    for (size_t i = 1; i < nmemb; i++) {
        memcpy(tmp, b + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(b + (j-1) * size, tmp) > 0) {
            memcpy(b + j * size, b + (j-1) * size, size);
            j--;
        }
        memcpy(b + j * size, tmp, size);
    }
    free(tmp);
}

void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size, int (*cmp)(const void *, const void *))
{
    const uint8_t *b = base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = cmp(key, b + mid * size);
        if (r == 0) return (void *)(b + mid * size);
        if (r < 0) hi = mid;
        else       lo = mid + 1;
    }
    return (void *)0;
}

/*  rand (xorshift64)  */
static uint64_t g_rand_state = 0xdeadbeefcafe1234ULL;

void srand(unsigned int seed)
{
    g_rand_state = (uint64_t)seed ^ 0xdeadbeef00000000ULL;
    if (!g_rand_state) g_rand_state = 1;
}

int rand(void)
{
    g_rand_state ^= g_rand_state << 13;
    g_rand_state ^= g_rand_state >> 7;
    g_rand_state ^= g_rand_state << 17;
    return (int)(g_rand_state & (uint64_t)RAND_MAX);
}

/*  getenv (no environment support yet — always NULL)  */
char *getenv(const char *name)
{
    (void)name;
    return (char *)0;
}

int  abs(int n)  { return n < 0 ? -n : n; }
long labs(long n){ return n < 0 ? -n : n; }

/*
 * strtod — parses [sign] digits [. digits] [(e|E) [sign] digits],
 * also accepting "inf"/"infinity"/"nan" (case-insensitive), matching
 * the C standard's requirements. Precision is "good enough" (uses
 * our own pow() for the exponent scaling) rather than
 * bit-for-bit-correctly-rounded like a production libm's strtod —
 * fine for a scripting language parsing source-code number literals.
 */
double strtod(const char *s, char **end)
{
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;

    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }

    /* inf / infinity / nan */
    if ((p[0]=='i'||p[0]=='I') && (p[1]=='n'||p[1]=='N') && (p[2]=='f'||p[2]=='F')) {
        const char *q = p + 3;
        if ((q[0]=='i'||q[0]=='I') && (q[1]=='n'||q[1]=='N') &&
            (q[2]=='i'||q[2]=='I') && (q[3]=='t'||q[3]=='T') &&
            (q[4]=='y'||q[4]=='Y')) {
            q += 5;
        }
        if (end) *end = (char *)q;
        return neg ? -HUGE_VAL : HUGE_VAL;
    }
    if ((p[0]=='n'||p[0]=='N') && (p[1]=='a'||p[1]=='A') && (p[2]=='n'||p[2]=='N')) {
        if (end) *end = (char *)(p + 3);
        double zero = 0.0;
        return zero / zero; /* NaN */
    }

    const char *digits_start = p;
    double intpart = 0.0;
    while (isdigit((unsigned char)*p)) { intpart = intpart * 10.0 + (*p - '0'); p++; }

    double frac = 0.0, scale = 1.0;
    if (*p == '.') {
        p++;
        while (isdigit((unsigned char)*p)) {
            frac = frac * 10.0 + (*p - '0');
            scale *= 10.0;
            p++;
        }
    }

    if (p == digits_start && *p != '.') {
        /* no digits at all consumed — not a valid number */
        if (end) *end = (char *)s;
        return 0.0;
    }

    double result = intpart + frac / scale;

    if (*p == 'e' || *p == 'E') {
        const char *exp_start = p;
        p++;
        int exp_neg = 0;
        if (*p == '+' || *p == '-') { exp_neg = (*p == '-'); p++; }
        if (isdigit((unsigned char)*p)) {
            int exp_val = 0;
            while (isdigit((unsigned char)*p)) { exp_val = exp_val * 10 + (*p - '0'); p++; }
            result *= pow(10.0, exp_neg ? -exp_val : exp_val);
        } else {
            p = exp_start; /* 'e'/'E' wasn't actually an exponent */
        }
    }

    if (end) *end = (char *)p;
    return neg ? -result : result;
}