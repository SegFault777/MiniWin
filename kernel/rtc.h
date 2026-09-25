#ifndef RTC_H
#define RTC_H
#include "io.h"

/* ============================================================
 * CMOS real-time clock -- the same battery-backed chip that's kept
 * every PC's clock running since the original IBM AT, read through two
 * ports (0x70 selects a register, 0x71 reads/writes it). No network,
 * no NTP, no geolocation: just genuine hardware time, the way every OS
 * bootstraps its clock before it has anything fancier to sync against.
 * ============================================================ */

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static inline u8 cmos_read(u8 reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static inline int cmos_update_in_progress(void) {
    return cmos_read(0x0A) & 0x80;
}

typedef struct {
    int second, minute, hour;  /* hour is always normalized to 24-hour, 0-23 */
    int day, month, year;      /* year is a full 4-digit year */
    int weekday;                /* 0=Sunday .. 6=Saturday */
} rtc_time_t;

static inline u8 bcd_to_bin(u8 v) { return (u8)((v & 0x0F) + ((v >> 4) * 10)); }

/* Sakamoto's algorithm: a small, well-known closed-form way to get the
 * day of week from a date, used instead of trusting the CMOS's own
 * weekday register (which is notorious for being wired up inconsistently
 * across real BIOS/hardware vendors -- computing it ourselves from the
 * y/m/d we already trust sidesteps that whole mess). */
static int compute_weekday(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void rtc_read(rtc_time_t *out) {
    /* Wait out any in-progress update so we don't catch the clock
     * mid-tick and read a torn value -- bounded, same busy-wait
     * philosophy as every other hardware poll in this kernel. In
     * practice this almost always exits on the very first check: the
     * update-in-progress flag is only set for a few hundred
     * microseconds, once a second. */
    for (u32 i = 0; i < 1000000 && cmos_update_in_progress(); i++) { }

    u8 sec = cmos_read(0x00);
    u8 min = cmos_read(0x02);
    u8 hour = cmos_read(0x04);
    u8 day = cmos_read(0x07);
    u8 month = cmos_read(0x08);
    u8 year = cmos_read(0x09);
    u8 status_b = cmos_read(0x0B);

    int is_binary = status_b & 0x04;
    int is_24hr = status_b & 0x02;
    int pm = hour & 0x80; /* only meaningful in 12-hour mode; harmless to compute otherwise */
    hour &= 0x7F;

    if (!is_binary) {
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        hour = bcd_to_bin(hour);
        day = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year = bcd_to_bin(year);
    }
    if (!is_24hr) {
        hour %= 12;
        if (pm) hour += 12;
    }

    out->second = sec;
    out->minute = min;
    out->hour = hour;
    out->day = day;
    out->month = month;
    out->year = 2000 + year; /* CMOS only gives 2 digits; this project's
                               * timeframe makes "assume 21st century" a
                               * safe bet rather than bothering with the
                               * inconsistently-implemented century register */
    out->weekday = compute_weekday(out->year, out->month, out->day);
}

static int rtc_is_leap_year(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}
static int rtc_days_in_month(int y, int m) {
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && rtc_is_leap_year(y)) return 29;
    return dim[m - 1];
}

/* Applies a whole-hour UTC offset to a reading, correctly rolling the
 * date forward or back across a midnight boundary (and across month/
 * year boundaries too) when the offset pushes the hour out of 0-23.
 * There's no automatic geolocation-based timezone here -- this kernel
 * has no IP/DNS/HTTP stack yet to ask anyone where it is -- so
 * offset_hours comes from a manual setting (SETTING.MWP > SYSTEM >
 * Time Zone) instead. */
static rtc_time_t rtc_apply_offset(rtc_time_t t, int offset_hours) {
    int h = t.hour + offset_hours;
    if (h >= 24) {
        h -= 24;
        t.day += 1;
        if (t.day > rtc_days_in_month(t.year, t.month)) {
            t.day = 1;
            t.month += 1;
            if (t.month > 12) { t.month = 1; t.year += 1; }
        }
    } else if (h < 0) {
        h += 24;
        t.day -= 1;
        if (t.day < 1) {
            t.month -= 1;
            if (t.month < 1) { t.month = 12; t.year -= 1; }
            t.day = rtc_days_in_month(t.year, t.month);
        }
    }
    t.hour = h;
    t.weekday = compute_weekday(t.year, t.month, t.day);
    return t;
}

#endif
