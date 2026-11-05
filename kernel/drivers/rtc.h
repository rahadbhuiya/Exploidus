#pragma once
#include <stdint.h>

/*
 * kernel/drivers/rtc.h — CMOS Real-Time-Clock reader.
 * Reads the battery-backed CMOS RTC (ports 0x70/0x71) for real
 * calendar date/time. This is the classic, well-documented technique
 * (same one virtually every hobby OS tutorial uses) — a single
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