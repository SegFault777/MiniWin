/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef USB_H
#define USB_H
#include "../io.h"
#include "../serial.h"
#include "../pci.h"
#include "../memmap.h"
#include "../delay.h"

/* usb/usb.h -- what every USB piece shares: the device table, the setup-packet helper, a millisecond clock and the few
 * constants the three host-controller drivers (xhci.h, ehci.h, ohci.h) and the enumerator (core.h) agree on.
 *
 * The shape of the stack, bottom to top:
 *      xhci.h / ehci.h / ohci.h   a host controller driver each: bring the controller up, find devices on its root ports,
 *                                 do ONE kind of control transfer and keep ONE interrupt-IN endpoint polled per device
 *      core.h                     enumeration (address, descriptors, configuration) and the poll that drives it all
 *      hid.h                      the class driver: boot keyboards, boot mice and report-descriptor mice/tablets
 * Everything is polled (no interrupts), identity-mapped (a pointer IS the bus address) and bounded (every wait has a
 * timeout, so a dead controller costs a boot-time pause, never a hang). External hubs and mass storage are not implemented. */

#define USB_HC_NONE 0
#define USB_HC_OHCI 1
#define USB_HC_EHCI 2
#define USB_HC_XHCI 3

#define USB_SPEED_FULL  1       /* the numbers xHCI uses in PORTSC and in the slot context; the others convert to them */
#define USB_SPEED_LOW   2
#define USB_SPEED_HIGH  3
#define USB_SPEED_SUPER 4

#define USB_MAX_DEVS    12
#define USB_CBUF_SIZE   256

#define HID_NONE   0
#define HID_KBD    1            /* boot-protocol keyboard */
#define HID_MOUSE  2            /* relative pointer (boot protocol or report descriptor) */
#define HID_TABLET 3            /* absolute pointer */

typedef struct {
    u8  type;                   /* USB_HC_* */
    u8  idx;                    /* 0..MW_USB_HC_MAX-1: also picks the DMA slice */
    u8  bus, slot, func;
    u8  nports;
    u32 mmio;                   /* register base (BAR0) */
    u32 dma;                    /* this controller's 64 KB slice */
} usb_hc_t;

/* One bit-field of a HID input report (see hid.h's report-descriptor parser). */
typedef struct { u16 off; u8 size; u8 rel; i32 lmin, lmax; } hid_field_t;

typedef struct {
    u8  present;
    u8  hc;                     /* index into usb_hcs[] */
    u8  port;                   /* root-hub port, 0-based */
    u8  speed;
    u8  addr;                   /* USB address (ehci/ohci) */
    u8  slot;                   /* xHCI slot id */
    u16 mps0;                   /* endpoint 0 max packet size */
    u16 vid, pid;
    u8  cls;
    u8  hid;                    /* HID_* */
    u8  iface, cfg;
    u8  ep;                     /* interrupt-IN endpoint number */
    u16 ep_mps;
    u8  ep_interval;
    u8  boot;                   /* 1 = boot protocol (fixed report layouts) */
    u8  rid;                    /* report id the fields live in (0 = none) */
    u16 rlen;                   /* bytes to ask the endpoint for */
    hid_field_t fx, fy, fwheel;
    u16 btn_off; u8 btn_cnt;
    u8  last[8];                /* keyboard: previous report, for press detection */
    u8  rep_key;                /* keyboard: key being auto-repeated (HID usage), 0 = none */
    u32 rep_next;               /* keyboard: ms timestamp of its next repeat */
    u8  dead;
} usb_dev_t;

static usb_hc_t  usb_hcs[MW_USB_HC_MAX];
static int       usb_hc_count = 0;
static usb_dev_t usb_devs[USB_MAX_DEVS];

/* The scratch buffer every control transfer's data stage goes through: static (so it has a fixed, known address the
 * controller can DMA to) and aligned so it never straddles a page. 256 bytes covers every descriptor we ask for. */
static u8 usb_cbuf[USB_CBUF_SIZE] __attribute__((aligned(64)));

#define USB_BARRIER() __asm__ volatile ("" ::: "memory")

static inline u32  usb_rd32(u32 a)          { return *(volatile u32 *)a; }
static inline void usb_wr32(u32 a, u32 v)   { *(volatile u32 *)a = v; }
static inline u16  usb_rd16(u32 a)          { return *(volatile u16 *)a; }
static inline u8   usb_rd8(u32 a)           { return *(volatile u8 *)a; }
static inline void usb_zero(void *p, u32 n) { volatile u8 *b = (volatile u8 *)p; for (u32 i = 0; i < n; i++) b[i] = 0; }
static inline void usb_copy(void *d, const void *s, u32 n) { u8 *a = (u8 *)d; const u8 *b = (const u8 *)s; for (u32 i = 0; i < n; i++) a[i] = b[i]; }

/* A millisecond clock built from the PIT: usb_poll() calls it every pass, and the PIT's 16-bit counter wraps every 55 ms,
 * so as long as it is called at least that often the sum is exact (a long stall just makes key repeat late, nothing worse). */
static u32 usb_now_ms_v = 0;
static u32 usb_now_ticks = 0;
static u16 usb_now_last = 0;
static inline u32 usb_now_ms(void) {
    u16 now = pit_read();
    usb_now_ticks += (u16)(usb_now_last - now);
    usb_now_last = now;
    u32 ms = usb_now_ticks / 1193u;
    usb_now_ticks -= ms * 1193u;
    usb_now_ms_v += ms;
    return usb_now_ms_v;
}

/* The 8-byte SETUP packet, built once, used by all three controllers. */
static inline void usb_setup_pkt(u8 *p, u8 bm, u8 req, u16 val, u16 idx, u16 len) {
    p[0] = bm; p[1] = req; p[2] = (u8)val; p[3] = (u8)(val >> 8);
    p[4] = (u8)idx; p[5] = (u8)(idx >> 8); p[6] = (u8)len; p[7] = (u8)(len >> 8);
}

/* log helpers: "[USB] ..." lines on the serial port are how a new controller gets diagnosed on real hardware */
static inline void usb_log(const char *s) { serial_puts("[USB] "); serial_puts(s); serial_putc('\n'); }
static inline void usb_log_hex(const char *s, u32 v) { serial_puts("[USB] "); serial_puts(s); serial_put_hex32(v); serial_putc('\n'); }

#endif
