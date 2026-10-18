/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef DELAY_H
#define DELAY_H
#include "io.h"

/* delay.h -- wall-clock waits from the PIT (the 8254 timer), for hardware that has to be given time: a floppy motor spinning
 * up, a head settling. Channel 0 is left by the BIOS free-running as a 16-bit down-counter at 1,193,182 Hz (mode 3, reload
 * 65536); latching and reading it never disturbs it, so this works with interrupts off and under any hypervisor. Waits
 * accumulate elapsed ticks, so they stay correct across the counter's wrap-around. */
static inline u16 pit_read(void) {
    outb(0x43, 0x00);                          /* latch channel 0's count */
    u16 lo = inb(0x40);
    u16 hi = inb(0x40);
    return (u16)(lo | (hi << 8));
}
static void delay_ms(u32 ms) {
    u32 want = ms * 1193u + ms / 5u;           /* ticks: ms * 1193.18 */
    u32 got = 0;
    u16 last = pit_read();
    while (got < want) {
        u16 now = pit_read();
        got += (u16)(last - now);              /* it counts DOWN */
        last = now;
    }
}
#endif
