#ifndef RTL8139_H
#define RTL8139_H
#include "io.h"
#include "pci.h"
#include "serial.h"
#include "nic.h"

/* ============================================================
 * Realtek RTL8139 driver -- the single most-documented, most-cloned
 * NIC in hobby-OS history, and for good reason: it's entirely
 * port-I/O-addressed (no MMIO, no BAR mapping headaches), the receive
 * path is one contiguous ring buffer the card DMAs into on its own, and
 * transmit is four fire-and-forget descriptor slots. No interrupts are
 * used here -- everything is polled from the kernel's existing main
 * loop, same as the mouse and keyboard already are.
 * ============================================================ */

#define RTL_VENDOR_ID 0x10EC
#define RTL_DEVICE_ID 0x8139

/* Register offsets from the I/O base (BAR0 with the low "I/O space" bit
 * masked off). */
#define RTL_IDR0     0x00   /* MAC address, 6 bytes */
#define RTL_TSD0     0x10   /* transmit status, descriptor 0 (TSD1-3 follow, 4 bytes apart) */
#define RTL_TSAD0    0x20   /* transmit start address, descriptor 0 (physical) */
#define RTL_RBSTART  0x30   /* receive ring buffer start address (physical) */
#define RTL_CR       0x37   /* command register */
#define RTL_CAPR     0x38   /* current address of packet read (software's RX pointer, minus 0x10 fudge) */
#define RTL_IMR      0x3C   /* interrupt mask -- left mostly zeroed, we poll instead */
#define RTL_ISR      0x3E   /* interrupt status -- also used for polling: bits latch even with IMR=0 */
#define RTL_TCR      0x40
#define RTL_RCR      0x44
#define RTL_CONFIG1  0x52

#define RTL_CR_RST   0x10
#define RTL_CR_RE    0x08
#define RTL_CR_TE    0x04
#define RTL_CR_BUFE  0x01   /* RX buffer empty */

#define RTL_ISR_ROK  0x0001
#define RTL_ISR_TOK  0x0004

/* RX ring: 8K + 16 (header slack) + 1500 (max frame) is the classic
 * "don't think too hard about wraparound" size recommended everywhere
 * this chip is documented; the extra room means a packet landing near
 * the end of the ring never needs to be split across the wrap point. */
#define RTL_RX_BUF_LEN (8192 + 16 + 1500)
static u8 rtl_rx_buf[RTL_RX_BUF_LEN] __attribute__((aligned(4)));

#define RTL_TX_SLOTS 4
#define RTL_TX_BUF_LEN 1536
static u8 rtl_tx_buf[RTL_TX_SLOTS][RTL_TX_BUF_LEN] __attribute__((aligned(4)));

static u16 rtl_io_base;
static u32 rtl_rx_offset;     /* our software read pointer into rtl_rx_buf */
static int rtl_tx_next;       /* which of the 4 TX slots to use next, round-robin */

static int rtl8139_send(const u8 *frame, u16 len) {
    if (len > RTL_TX_BUF_LEN) return 0; /* not going to fragment; caller's problem */

    int slot = rtl_tx_next;
    rtl_tx_next = (rtl_tx_next + 1) % RTL_TX_SLOTS;

    /* Ethernet has a 60-byte minimum frame length (64 with the 4-byte
     * FCS the hardware appends); pad short frames with zeros so the
     * card doesn't reject them. */
    u8 *buf = rtl_tx_buf[slot];
    for (u16 i = 0; i < len; i++) buf[i] = frame[i];
    u16 tx_len = len;
    while (tx_len < 60) buf[tx_len++] = 0;

    outl(rtl_io_base + RTL_TSAD0 + (u16)(slot * 4), (u32)(u32)buf);
    /* Writing the length to TSD also kicks off transmission. Bits 0-12
     * are the length; that's all we need to set. */
    outl(rtl_io_base + RTL_TSD0 + (u16)(slot * 4), tx_len);

    /* Poll (briefly, bounded) for TOK on this descriptor so a caller
     * doing several sends in a row doesn't outrun the hardware and
     * stomp a slot that's still in flight. This is a busy-wait like
     * everything else in this kernel, not a real timeout in seconds. */
    for (u32 i = 0; i < 200000; i++) {
        if (inl(rtl_io_base + RTL_TSD0 + (u16)(slot * 4)) & 0x8000) break;
    }
    return 1;
}

static int rtl8139_recv(u8 *out, u16 max_len, u16 *out_len) {
    if (inb(rtl_io_base + RTL_CR) & RTL_CR_BUFE) return 0; /* ring is empty */

    /* Each received frame is prefixed by the card with a 4-byte header:
     * u16 status, u16 length (length includes the 4-byte FCS, which we
     * don't want to hand upward). */
    u8 *hdr = rtl_rx_buf + rtl_rx_offset;
    u16 status = (u16)(hdr[0] | (hdr[1] << 8));
    u16 frame_len = (u16)(hdr[2] | (hdr[3] << 8));

    if (!(status & RTL_ISR_ROK) || frame_len < 4 || frame_len > 1518) {
        /* Something's desynced (shouldn't happen in normal operation) --
         * rather than trust garbage and walk off into the weeds, just
         * report "nothing new" and leave the pointer alone. A real
         * driver would reset the whole ring here; this is a polling
         * hobby-OS driver, so "do nothing and hope the next poll is
         * sane" is an acceptable, honest limitation. */
        return 0;
    }

    u16 payload_len = (u16)(frame_len - 4); /* drop the FCS */
    u16 copy_len = (payload_len < max_len) ? payload_len : max_len;
    for (u16 i = 0; i < copy_len; i++) {
        out[i] = rtl_rx_buf[(rtl_rx_offset + 4 + i) % RTL_RX_BUF_LEN];
    }
    *out_len = copy_len;

    /* Advance past this frame (header + data), round up to a 4-byte
     * boundary (the card does the same internally), and wrap. */
    u32 next = rtl_rx_offset + 4 + frame_len;
    next = (next + 3) & ~((u32)3);
    next %= RTL_RX_BUF_LEN;
    rtl_rx_offset = next;

    /* CAPR wants "read pointer minus 16 bytes" by convention (a quirk
     * of this exact chip, documented everywhere it's been reverse
     * engineered) -- get this wrong and the ring desyncs after the
     * first wraparound. */
    outw(rtl_io_base + RTL_CAPR, (u16)(rtl_rx_offset - 16));
    return 1;
}

static int rtl8139_init(void) {
    pci_device_t *dev = pci_find_device(RTL_VENDOR_ID, RTL_DEVICE_ID);
    if (!dev) return 0;

    pci_enable_bus_mastering(dev->bus, dev->slot, dev->func);

    if (!(dev->bar0 & 0x1)) {
        serial_puts("[RTL8139] BAR0 is not I/O-mapped, giving up\n");
        return 0;
    }
    rtl_io_base = (u16)(dev->bar0 & 0xFFFC);

    outb(rtl_io_base + RTL_CONFIG1, 0x00); /* power on */

    outb(rtl_io_base + RTL_CR, RTL_CR_RST); /* software reset */
    for (u32 i = 0; i < 1000000; i++) {
        if (!(inb(rtl_io_base + RTL_CR) & RTL_CR_RST)) break;
    }

    rtl_rx_offset = 0;
    rtl_tx_next = 0;
    outl(rtl_io_base + RTL_RBSTART, (u32)(u32)rtl_rx_buf);

    outw(rtl_io_base + RTL_IMR, 0x0000); /* polled, not interrupt-driven */

    /* AB (accept broadcast) | AM (accept multicast) | APM (accept our own
     * unicast) | WRAP -- the standard "just give me everything sane"
     * config used by essentially every hobby driver for this chip. */
    outl(rtl_io_base + RTL_RCR, 0x0000000F | (1u << 7));

    outb(rtl_io_base + RTL_CR, RTL_CR_RE | RTL_CR_TE);

    for (int i = 0; i < 6; i++) nic.mac[i] = inb(rtl_io_base + RTL_IDR0 + i);

    nic.present = 1;
    nic.chip_name = "RTL8139";
    nic.send = rtl8139_send;
    nic.recv = rtl8139_recv;

    serial_puts("[RTL8139] initialized, MAC=");
    for (int i = 0; i < 6; i++) {
        serial_put_hex8(nic.mac[i]);
        if (i < 5) serial_putc(':');
    }
    serial_putc('\n');
    return 1;
}

#endif
