#ifndef SERIAL_H
#define SERIAL_H
#include "io.h"

/* ============================================================
 * COM1 serial port -- not part of the desktop OS itself, just a debug
 * output the VGA framebuffer can't give us: a plain scrolling text log
 * that QEMU can capture straight to a file (`-serial file:...` or
 * `-serial stdio`) without touching the graphical display at all.
 * Every hobby-OS driver bring-up leans on this exact trick because
 * there's nothing simpler to bootstrap: no interrupts, no buffering,
 * just a UART and a busy-wait.
 * ============================================================ */
#define COM1 0x3F8

static inline void serial_init(void) {
    outb(COM1 + 1, 0x00);    /* disable interrupts */
    outb(COM1 + 3, 0x80);    /* enable DLAB (set baud rate divisor) */
    outb(COM1 + 0, 0x03);    /* divisor low byte: 3 -> 38400 baud */
    outb(COM1 + 1, 0x00);    /* divisor high byte */
    outb(COM1 + 3, 0x03);    /* 8 bits, no parity, 1 stop bit */
    outb(COM1 + 2, 0xC7);    /* enable FIFO, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B);    /* IRQs disabled, RTS/DSR set */
}

static inline int serial_tx_ready(void) {
    return inb(COM1 + 5) & 0x20;
}

static inline void serial_putc(char c) {
    while (!serial_tx_ready()) { }
    outb(COM1, (u8)c);
}

static inline void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r'); /* plain terminals want CRLF */
        serial_putc(*s++);
    }
}

/* Minimal hex-byte/word/dword printers -- no printf here, this is a
 * freestanding kernel with no libc, so every debug line gets built by
 * hand out of pieces this small. */
static inline void serial_put_hex_nibble(u8 n) {
    serial_putc(n < 10 ? (char)('0' + n) : (char)('A' + (n - 10)));
}
static inline void serial_put_hex8(u8 v) {
    serial_put_hex_nibble(v >> 4);
    serial_put_hex_nibble(v & 0xF);
}
static inline void serial_put_hex16(u16 v) {
    serial_put_hex8((u8)(v >> 8));
    serial_put_hex8((u8)(v & 0xFF));
}
static inline void serial_put_hex32(u32 v) {
    serial_put_hex16((u16)(v >> 16));
    serial_put_hex16((u16)(v & 0xFFFF));
}

#endif
