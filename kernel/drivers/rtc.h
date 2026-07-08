#pragma once
#include <stdint.h>

/*
 * kernel/drivers/rtc.h — CMOS Real-Time-Clock reader.
 *
 * Reads the battery-backed CMOS RTC (ports 0x70/0x71) for real
 * calendar date/time. This is the classic, well-documented technique
 * (same one virtually every hobby OS tutorial uses) — a single
 * on-demand read, no ongoing interrupt/periodic-update complexity.
 * Handles the BCD-vs-binary register format and the "update in
 * progress" race (retries if the RTC is mid-tick while read).
 *
 * Didn't exist before this — Exploidus previously had no wall-clock
 * time source at all, only uptime() (ticks since boot). Needed for
 * any ported language's time()/os.date() to return real dates
 * instead of just "seconds since this OS booted".
 */

typedef struct {
    uint16_t year;   /* full year, e.g. 2026 */
    uint8_t  month;  /* 1-12 */
    uint8_t  day;    /* 1-31 */
    uint8_t  hour;   /* 0-23 */
    uint8_t  minute; /* 0-59 */
    uint8_t  second; /* 0-59 */
} rtc_time_t;

/* Reads the current CMOS RTC date/time (UTC — CMOS clocks are
 * conventionally kept in UTC or local time depending on host
 * configuration; Exploidus treats it as UTC, no timezone support). */
void rtc_read(rtc_time_t *out);