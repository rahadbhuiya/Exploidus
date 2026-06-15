/*
 * Exploidus minimal stdlib.c
 */
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

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