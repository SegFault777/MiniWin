#ifndef SPEAKER_H
#define SPEAKER_H
#include "io.h"

/* ============================================================
 * PC speaker -- the tiny piezo buzzer that's been bolted to x86
 * motherboards since 1981, driven straight through the PIT
 * (Programmable Interval Timer, the same chip that would drive a
 * hardware clock tick if this kernel bothered to use interrupts) and a
 * couple of bits on keyboard-controller port 0x61. No sound card, no
 * mixer, no driver stack -- just a square wave, generated the hard way,
 * because that's the only way available.
 * ============================================================ */

/* PIT channel 2, configured for square-wave output. The chip's input
 * clock is a fixed ~1.193182 MHz no matter what CPU you're on, so this
 * divisor math is the same on real 1981 hardware and inside QEMU. */
static inline void speaker_set_frequency(u32 hz) {
    if (hz == 0) hz = 1; /* dividing by zero would be a genuinely different kind of silence */
    u32 divisor = 1193182 / hz;
    outb(0x43, 0xB6);                          /* channel 2, lobyte/hibyte, mode 3 */
    outb(0x42, (u8)(divisor & 0xFF));
    outb(0x42, (u8)((divisor >> 8) & 0xFF));
}

static inline void speaker_on(void) {
    u8 tmp = inb(0x61);
    outb(0x61, tmp | 0x03); /* bit0: gate PIT channel 2, bit1: route it to the speaker */
}

static inline void speaker_off(void) {
    u8 tmp = inb(0x61);
    outb(0x61, tmp & 0xFC);
}

/* Same busy-wait-nop timing philosophy as the rest of this kernel (see
 * kernel.c's delay()) -- there's no real timer interrupt to sleep
 * against, so "how long is this beep" really means "how many times can
 * this CPU say nop before we get bored." Approximate on real hardware,
 * consistent enough on the QEMU speed this project is tuned for. */
static inline void speaker_hold(u32 loops) {
    while (loops--) { __asm__ volatile ("nop"); }
}

/* The one alert sound in this whole OS: a short, slightly urgent beep
 * for Notepad's Warning dialog. Not a dial tone, not a doorbell --
 * just enough pitch to say "hey, unsaved changes." Held generously
 * long (and only ever called once per dialog open) since this is a
 * plain nop-spin duration with no real timer behind it: faster host
 * CPUs burn through the loop faster, so erring long keeps it audible
 * across a wider range of machines instead of risking an inaudible
 * blip on anything quicker than our own test box. */
static inline void beep_warning(void) {
    speaker_set_frequency(950);
    speaker_on();
    speaker_hold(3000000);
    speaker_off();
}

#endif
