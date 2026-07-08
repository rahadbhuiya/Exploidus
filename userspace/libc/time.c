#include "time.h"
#include "syscall.h"
#include <string.h>
#include <stdio.h>

/*
 * Civil-calendar <-> days-since-epoch conversion using the standard
 * Howard Hinnant algorithm (public domain, widely used — correct
 * proleptic Gregorian calendar math including all leap-year rules).
 */
static long days_from_civil(long y, int m, int d)
{
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static void civil_from_days(long z, long *y, int *m, int *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp + (mp < 10 ? 3u : (unsigned)-9);
    *y = yy + (long)(mm <= 2);
    *m = (int)mm;
    *d = (int)dd;
}

time_t time(time_t *out)
{
    rtc_time_t rtc;
    rtc_read(&rtc);

    long days = days_from_civil(rtc.year, rtc.month, rtc.day);
    time_t t = (time_t)days * 86400
             + (time_t)rtc.hour * 3600
             + (time_t)rtc.minute * 60
             + (time_t)rtc.second;
    if (out) *out = t;
    return t;
}

double difftime(time_t a, time_t b)
{
    return (double)(a - b);
}

static struct tm g_tm_buf; /* like glibc's static-buffer gmtime/localtime */

static struct tm *time_to_tm(const time_t *t, struct tm *r)
{
    long days = (long)(*t / 86400);
    long rem  = (long)(*t % 86400);
    if (rem < 0) { rem += 86400; days -= 1; }

    long y; int m, d;
    civil_from_days(days, &y, &m, &d);

    r->tm_year = (int)(y - 1900);
    r->tm_mon  = m - 1;
    r->tm_mday = d;
    r->tm_hour = (int)(rem / 3600);
    r->tm_min  = (int)((rem % 3600) / 60);
    r->tm_sec  = (int)(rem % 60);

    /* 1970-01-01 was a Thursday (wday 4) */
    long wd = (days % 7 + 7 + 4) % 7;
    r->tm_wday = (int)wd;

    long jan1 = days_from_civil(y, 1, 1);
    r->tm_yday = (int)(days - jan1);
    r->tm_isdst = 0;
    return r;
}

struct tm *gmtime(const time_t *t)    { return time_to_tm(t, &g_tm_buf); }
struct tm *localtime(const time_t *t) { return time_to_tm(t, &g_tm_buf); } /* no tz data */

time_t mktime(struct tm *tm)
{
    long days = days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    time_t t = (time_t)days * 86400
             + (time_t)tm->tm_hour * 3600
             + (time_t)tm->tm_min * 60
             + (time_t)tm->tm_sec;
    /* Normalize tm's derived fields (wday/yday) to match, like the
     * real mktime() does. */
    time_to_tm(&t, tm);
    return t;
}

clock_t clock(void)
{
    /* 10ms resolution, matches the real scheduler tick rate — not
     * padded with fake precision the hardware doesn't actually have. */
    return (clock_t)uptime();
}


/* strftime — common specifiers only, see time.h for exact list     */


static const char *const g_month_full[] = {
    "January","February","March","April","May","June",
    "July","August","September","October","November","December"
};
static const char *const g_month_abbr[] = {
    "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char *const g_day_full[] = {
    "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
};
static const char *const g_day_abbr[] = {
    "Sun","Mon","Tue","Wed","Thu","Fri","Sat"
};

size_t strftime(char *s, size_t maxsize, const char *fmt, const struct tm *tm)
{
    size_t out = 0;
    char tmp[32];

#define PUT_STR(str) do { \
        const char *_p = (str); \
        while (*_p) { if (out + 1 >= maxsize) return 0; s[out++] = *_p++; } \
    } while (0)
#define PUT_NUM(fmtspec, val) do { \
        int _n = snprintf(tmp, sizeof(tmp), fmtspec, (int)(val)); \
        (void)_n; PUT_STR(tmp); \
    } while (0)

    while (*fmt) {
        if (*fmt != '%') {
            if (out + 1 >= maxsize) return 0;
            s[out++] = *fmt++;
            continue;
        }
        fmt++; /* skip '%' */
        switch (*fmt) {
            case 'Y': PUT_NUM("%d", tm->tm_year + 1900); break;
            case 'y': PUT_NUM("%02d", (tm->tm_year + 1900) % 100); break;
            case 'm': PUT_NUM("%02d", tm->tm_mon + 1); break;
            case 'd': PUT_NUM("%02d", tm->tm_mday); break;
            case 'H': PUT_NUM("%02d", tm->tm_hour); break;
            case 'I': { int h12 = tm->tm_hour % 12; if (h12 == 0) h12 = 12; PUT_NUM("%02d", h12); break; }
            case 'M': PUT_NUM("%02d", tm->tm_min); break;
            case 'S': PUT_NUM("%02d", tm->tm_sec); break;
            case 'j': PUT_NUM("%03d", tm->tm_yday + 1); break;
            case 'w': PUT_NUM("%d", tm->tm_wday); break;
            case 'p': PUT_STR(tm->tm_hour < 12 ? "AM" : "PM"); break;
            case 'A': PUT_STR(g_day_full[tm->tm_wday % 7]); break;
            case 'a': PUT_STR(g_day_abbr[tm->tm_wday % 7]); break;
            case 'B': PUT_STR(g_month_full[tm->tm_mon % 12]); break;
            case 'b': PUT_STR(g_month_abbr[tm->tm_mon % 12]); break;
            case 'x': /* MM/DD/YY */
                PUT_NUM("%02d", tm->tm_mon + 1); PUT_STR("/");
                PUT_NUM("%02d", tm->tm_mday); PUT_STR("/");
                PUT_NUM("%02d", (tm->tm_year + 1900) % 100);
                break;
            case 'X': /* HH:MM:SS */
                PUT_NUM("%02d", tm->tm_hour); PUT_STR(":");
                PUT_NUM("%02d", tm->tm_min);  PUT_STR(":");
                PUT_NUM("%02d", tm->tm_sec);
                break;
            case 'c': /* full date+time, e.g. "Mon Jan  2 15:04:05 2026" */
                PUT_STR(g_day_abbr[tm->tm_wday % 7]); PUT_STR(" ");
                PUT_STR(g_month_abbr[tm->tm_mon % 12]); PUT_STR(" ");
                PUT_NUM("%2d", tm->tm_mday); PUT_STR(" ");
                PUT_NUM("%02d", tm->tm_hour); PUT_STR(":");
                PUT_NUM("%02d", tm->tm_min);  PUT_STR(":");
                PUT_NUM("%02d", tm->tm_sec);  PUT_STR(" ");
                PUT_NUM("%d", tm->tm_year + 1900);
                break;
            case '%': PUT_STR("%"); break;
            default:
                /* Unsupported specifier — emit literally rather than
                 * silently dropping data, so it's visible if hit. */
                if (out + 2 >= maxsize) return 0;
                s[out++] = '%';
                s[out++] = *fmt;
                break;
        }
        fmt++;
    }

    if (out >= maxsize) return 0;
    s[out] = '\0';
    return out;

#undef PUT_STR
#undef PUT_NUM
}