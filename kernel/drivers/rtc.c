#include "rtc.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int rtc_update_in_progress(void)
{
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

#define BCD_TO_BIN(v) (((v) & 0x0F) + (((v) >> 4) * 10))

void rtc_read(rtc_time_t *out)
{
    uint8_t sec, min, hour, day, month, year, century = 20;
    uint8_t regB;

    /* Retry until we get two consecutive identical readings while not
     * mid-update — the standard, well-known way to avoid tearing a
     * read across the RTC's own internal tick. */
    uint8_t last_sec = 0xFF, last_min = 0xFF, last_hour = 0xFF;
    uint8_t last_day = 0xFF, last_month = 0xFF, last_year = 0xFF;

    do {
        while (rtc_update_in_progress()) { /* spin */ }

        sec   = cmos_read(0x00);
        min   = cmos_read(0x02);
        hour  = cmos_read(0x04);
        day   = cmos_read(0x07);
        month = cmos_read(0x08);
        year  = cmos_read(0x09);

        if (sec == last_sec && min == last_min && hour == last_hour &&
            day == last_day && month == last_month && year == last_year) {
            break;
        }
        last_sec = sec; last_min = min; last_hour = hour;
        last_day = day; last_month = month; last_year = year;
    } while (1);

    regB = cmos_read(0x0B);

    if (!(regB & 0x04)) { /* values are in BCD, convert to binary */
        sec   = BCD_TO_BIN(sec);
        min   = BCD_TO_BIN(min);
        hour  = BCD_TO_BIN(hour & 0x7F);
        day   = BCD_TO_BIN(day);
        month = BCD_TO_BIN(month);
        year  = BCD_TO_BIN(year);
    }

    if (!(regB & 0x02) && (hour & 0x80)) {
        /* 12-hour mode with PM bit set */
        hour = ((hour & 0x7F) + 12) % 24;
    }

    out->year   = (uint16_t)(century * 100 + year);
    out->month  = month;
    out->day    = day;
    out->hour   = hour;
    out->minute = min;
    out->second = sec;
}