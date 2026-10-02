#ifndef NETCLOCK_H
#define NETCLOCK_H
#include "io.h"
#include "rtc.h"

/* ============================================================
 * netclock.h -- a wall-clock to time network retries by.
 *
 * net_ticks (net.h) counts main-loop passes, and how long a pass takes depends entirely on the
 * machine: ~45/second under software-emulated QEMU, thousands per second under hardware
 * acceleration or on real hardware. Anything measured in ticks -- "give up after 4000" -- is
 * therefore a different number of SECONDS on every machine, which is how DNS started failing on
 * fast machines. Retry/timeout logic that means seconds should use this instead.
 *
 * Resolution is one second (the CMOS RTC), so "wait 3 seconds" fires 2-3 seconds in. Elapsed time is
 * computed modulo a day, so it survives midnight.
 * ============================================================ */
static inline u32 net_wall_seconds(void) {
    rtc_time_t t;
    rtc_read(&t);
    return (u32)t.hour * 3600u + (u32)t.minute * 60u + (u32)t.second;
}
static inline u32 net_wall_elapsed(u32 since) {
    return (net_wall_seconds() + 86400u - since) % 86400u;
}

#endif
