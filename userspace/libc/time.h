#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * time.h — real calendar time support, backed by the CMOS RTC
 * (kernel/drivers/rtc.c) rather than just uptime()-since-boot.
 *
 * Honest limitations:
 *   - No timezone support: localtime() == gmtime() always (UTC).
 *   - clock() has 10ms resolution (CLOCKS_PER_SEC=100), matching the
 *     actual scheduler tick rate — not padded with fake precision.
 *   - strftime() supports the common specifiers (Y y m d H M S j p A
 *     a B b w c x X %), not the full C99 set (no %z %Z %U %W %V %G
 *     %g %e — Lua's os.date rarely needs these in practice).
 */

typedef int64_t time_t;
typedef int64_t clock_t;

#define CLOCKS_PER_SEC 100

struct tm {
    int tm_sec;    /* 0-60 (61 for leap second, unused here) */
    int tm_min;    /* 0-59 */
    int tm_hour;   /* 0-23 */
    int tm_mday;   /* 1-31 */
    int tm_mon;    /* 0-11 */
    int tm_year;   /* years since 1900 */
    int tm_wday;   /* 0-6, Sunday = 0 */
    int tm_yday;   /* 0-365 */
    int tm_isdst;  /* always 0 — no DST support */
};

time_t time(time_t *out);
double difftime(time_t a, time_t b);
time_t mktime(struct tm *tm);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t); /* same as gmtime — no timezone data */
size_t strftime(char *s, size_t maxsize, const char *fmt, const struct tm *tm);
clock_t clock(void);