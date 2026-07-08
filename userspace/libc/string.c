/*
 * Exploidus minimal string.c
 */
#include "string.h"
#include "stdlib.h"
#include "syscall.h"

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    while (n-- && *src) *d++ = *src++;
    *d = 0;
    return dst;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : (char *)0;
}

char *strrchr(const char *s, int c)
{
    const char *last = (char *)0;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (char *)last;
}

char *strstr(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    if (!nlen) return (char *)hay;
    while (*hay) {
        if (strncmp(hay, needle, nlen) == 0) return (char *)hay;
        hay++;
    }
    return (char *)0;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static char *strtok_saved;
char *strtok(char *s, const char *delim)
{
    return strtok_r(s, delim, &strtok_saved);
}

char *strtok_r(char *s, const char *delim, char **saveptr)
{
    if (!s) s = *saveptr;
    while (*s && strchr(delim, *s)) s++;
    if (!*s) { *saveptr = s; return (char *)0; }
    char *start = s;
    while (*s && !strchr(delim, *s)) s++;
    if (*s) { *s++ = 0; }
    *saveptr = s;
    return start;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;
    while (n--) {
        if (*p != *q) return *p - *q;
        p++; q++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = s;
    while (n--) {
        if (*p == (uint8_t)c) return (void *)p;
        p++;
    }
    return (void *)0;
}

long strtol(const char *s, char **end, int base)
{
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1]=='x'||s[1]=='X')) s += 2;
    long val = 0;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        val = val * base + d;
        s++;
    }
    if (end) *end = (char *)s;
    return neg ? -val : val;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long)strtol(s, end, base);
}

long long strtoll(const char *s, char **end, int base)
{
    return (long long)strtol(s, end, base);
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++) {
        for (const char *a = accept; *a; a++) {
            if (*s == *a) return (char *)s;
        }
    }
    return (char *)0;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (*s) {
        const char *a = accept;
        for (; *a; a++) if (*s == *a) break;
        if (!*a) break; /* s char not found in accept */
        n++; s++;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (*s) {
        const char *r = reject;
        for (; *r; r++) if (*s == *r) break;
        if (*r) break; /* s char found in reject */
        n++; s++;
    }
    return n;
}

/* Exploidus only has the "C" locale, so collation == byte comparison. */
int strcoll(const char *a, const char *b)
{
    return strcmp(a, b);
}

/* Minimal strerror — no per-errno message table, but returns a
 * stable, non-null string (Lua's luaL_fileresult just wants
 * something readable for error messages). */
static char g_strerror_buf[32];
char *strerror(int errnum)
{
    /* Reuse of sprintf-style formatting would need stdio.h here,
     * creating a header-order dependency — keep this self-contained
     * with simple manual itoa instead. */
    const char *prefix = "error ";
    size_t i = 0, n = 0;
    while (prefix[i]) { g_strerror_buf[n++] = prefix[i++]; }

    unsigned int v = (unsigned int)(errnum < 0 ? -errnum : errnum);
    char digits[12]; int nd = 0;
    if (v == 0) digits[nd++] = '0';
    while (v > 0 && nd < (int)sizeof(digits)) { digits[nd++] = (char)('0' + v % 10); v /= 10; }
    if (errnum < 0) g_strerror_buf[n++] = '-';
    while (nd > 0) g_strerror_buf[n++] = digits[--nd];
    g_strerror_buf[n] = '\0';
    return g_strerror_buf;
}