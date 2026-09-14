#ifndef E1000_H
#define E1000_H
#include "io.h"
#include "pci.h"
#include "serial.h"
#include "nic.h"

/* ============================================================
 * Intel e1000 (82540EM) driver -- QEMU's other extremely common default
 * NIC, and a genuinely different programming model from the RTL8139:
 * no port I/O at all, just a block of memory-mapped registers (BAR0)
 * and two descriptor rings (RX, TX) that the card walks on its own via
 * DMA. Since this kernel runs with paging disabled (flat, identity-
 * mapped 32-bit protected mode -- see boot/boot.asm), a physical
 * address and a plain C pointer are the same number, so MMIO here is
 * just "dereference a volatile pointer at the right offset." No
 * separate memory-mapping step is needed the way a paged kernel would
 * require.
 *
 * Registers/descriptor layouts below match Intel's public 8254x
 * software developer's manual; this is the same chip (and largely the
 * same register map) as the real 82540EM, 82545EM, etc. QEMU's "e1000"
 * device model emulates the 82540EM specifically.
 * ============================================================ */

#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_ID 0x100E   /* 82540EM -- what QEMU's -device e1000 presents */

/* Register byte offsets from the MMIO base (BAR0). */
#define E1000_REG_CTRL    0x0000
#define E1000_REG_STATUS  0x0008
#define E1000_REG_EERD    0x0014
#define E1000_REG_ICR     0x00C0
#define E1000_REG_IMS     0x00D0
#define E1000_REG_IMC     0x00D8
#define E1000_REG_RCTL    0x0100
#define E1000_REG_TCTL    0x0400
#define E1000_REG_TIPG    0x0410
#define E1000_REG_RDBAL   0x2800
#define E1000_REG_RDBAH   0x2804
#define E1000_REG_RDLEN   0x2808
#define E1000_REG_RDH     0x2810
#define E1000_REG_RDT     0x2818
#define E1000_REG_TDBAL   0x3800
#define E1000_REG_TDBAH   0x3804
#define E1000_REG_TDLEN   0x3808
#define E1000_REG_TDH     0x3810
#define E1000_REG_TDT     0x3818
#define E1000_REG_RAL0    0x5400
#define E1000_REG_RAH0    0x5404

#define E1000_CTRL_RST    (1u << 26)
#define E1000_CTRL_SLU    (1u << 6)   /* Set Link Up */

#define E1000_RCTL_EN     (1u << 1)
#define E1000_RCTL_UPE    (1u << 3)   /* unicast promiscuous -- accept our own unicast even if RAL/RAH lookup is fussy */
#define E1000_RCTL_BAM    (1u << 15)  /* broadcast accept */
#define E1000_RCTL_SECRC  (1u << 26)  /* strip Ethernet CRC before handing us the frame */
#define E1000_RCTL_BSIZE_2048 0       /* bits 17:16 = 00 with BSEX=0 -> 2048-byte buffers */

#define E1000_TCTL_EN     (1u << 1)
#define E1000_TCTL_PSP    (1u << 3)
#define E1000_TCTL_CT_SHIFT   4
#define E1000_TCTL_COLD_SHIFT 12

#define E1000_TXD_CMD_EOP  0x01
#define E1000_TXD_CMD_IFCS 0x02
#define E1000_TXD_CMD_RS   0x08
#define E1000_TXD_STAT_DD  0x01

#define E1000_RXD_STAT_DD  0x01
#define E1000_RXD_STAT_EOP 0x02

typedef struct {
    u32 addr_lo;
    u32 addr_hi;   /* always 0 -- this kernel is 32-bit, physical addresses fit in addr_lo alone */
    u16 length;
    u16 checksum;
    u8  status;
    u8  errors;
    u16 special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    u32 addr_lo;
    u32 addr_hi;
    u16 length;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} __attribute__((packed)) e1000_tx_desc_t;

#define E1000_NUM_RX_DESC 32
#define E1000_NUM_TX_DESC 8
#define E1000_BUF_LEN     2048

static e1000_rx_desc_t e1000_rx_ring[E1000_NUM_RX_DESC] __attribute__((aligned(16)));
static e1000_tx_desc_t e1000_tx_ring[E1000_NUM_TX_DESC] __attribute__((aligned(16)));
static u8 e1000_rx_buf[E1000_NUM_RX_DESC][E1000_BUF_LEN] __attribute__((aligned(16)));
static u8 e1000_tx_buf[E1000_NUM_TX_DESC][E1000_BUF_LEN] __attribute__((aligned(16)));

static u32 e1000_mmio_base;
static u32 e1000_rx_cur;
static u32 e1000_tx_cur;

static inline void e1000_write32(u32 offset, u32 value) {
    *(volatile u32 *)(e1000_mmio_base + offset) = value;
}
static inline u32 e1000_read32(u32 offset) {
    return *(volatile u32 *)(e1000_mmio_base + offset);
}

static int e1000_send(const u8 *frame, u16 len) {
    if (len > E1000_BUF_LEN) return 0;

    u32 slot = e1000_tx_cur;
    e1000_tx_cur = (e1000_tx_cur + 1) % E1000_NUM_TX_DESC;

    u8 *buf = e1000_tx_buf[slot];
    for (u16 i = 0; i < len; i++) buf[i] = frame[i];
    u16 tx_len = len;
    while (tx_len < 60) buf[tx_len++] = 0; /* Ethernet minimum frame length */

    e1000_tx_ring[slot].addr_lo = (u32)buf;
    e1000_tx_ring[slot].addr_hi = 0;
    e1000_tx_ring[slot].length = tx_len;
    e1000_tx_ring[slot].cso = 0;
    e1000_tx_ring[slot].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    e1000_tx_ring[slot].status = 0;
    e1000_tx_ring[slot].css = 0;
    e1000_tx_ring[slot].special = 0;

    e1000_write32(E1000_REG_TDT, (slot + 1) % E1000_NUM_TX_DESC);

    /* Poll (bounded) for the card to mark this descriptor done, same
     * "don't trust a promise, wait for the receipt" approach as the
     * RTL8139 driver's send(). */
    for (u32 i = 0; i < 200000; i++) {
        if (e1000_tx_ring[slot].status & E1000_TXD_STAT_DD) break;
    }
    return 1;
}

static int e1000_recv(u8 *out, u16 max_len, u16 *out_len) {
    e1000_rx_desc_t *d = &e1000_rx_ring[e1000_rx_cur];
    if (!(d->status & E1000_RXD_STAT_DD)) return 0; /* nothing new */

    u16 copy_len = (d->length < max_len) ? d->length : max_len;
    u8 *src = e1000_rx_buf[e1000_rx_cur];
    for (u16 i = 0; i < copy_len; i++) out[i] = src[i];
    *out_len = copy_len;

    /* Hand this descriptor back to the hardware: clear its status and
     * move the tail pointer up to (once again) include it in the ring
     * the card is allowed to write into. */
    d->status = 0;
    e1000_write32(E1000_REG_RDT, e1000_rx_cur);
    e1000_rx_cur = (e1000_rx_cur + 1) % E1000_NUM_RX_DESC;
    return 1;
}

static int e1000_init(void) {
    pci_device_t *dev = pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_ID);
    if (!dev) return 0;

    pci_enable_bus_mastering(dev->bus, dev->slot, dev->func);

    /* BAR0 here is memory-mapped: bit 0 clear means memory space (as
     * opposed to the RTL8139's I/O-space BAR0, which has bit 0 set).
     * Bits 1-3 describe address width/prefetchability, not part of the
     * base address, so mask off the low 4 bits regardless of their
     * value to recover the actual physical base. */
    if (dev->bar0 & 0x1) {
        serial_puts("[e1000] BAR0 is I/O-mapped, not memory-mapped -- unexpected, giving up\n");
        return 0;
    }
    e1000_mmio_base = dev->bar0 & 0xFFFFFFF0u;

    /* Full reset, then wait it out. There's no documented "reset done"
     * bit to poll on this family the way RTL8139 has one -- Intel's own
     * driver just waits ~1us minimum and re-reads CTRL, which will no
     * longer show the RST bit set once the card's internal reset
     * sequence completes. A bounded busy-wait loop covers this the same
     * way the rest of this kernel waits on hardware. */
    e1000_write32(E1000_REG_CTRL, e1000_read32(E1000_REG_CTRL) | E1000_CTRL_RST);
    for (u32 i = 0; i < 1000000; i++) {
        if (!(e1000_read32(E1000_REG_CTRL) & E1000_CTRL_RST)) break;
    }

    /* Disable every interrupt source -- polled driver, same philosophy
     * as RTL8139's. Reading ICR afterward clears any latched causes
     * left over from before the reset. */
    e1000_write32(E1000_REG_IMC, 0xFFFFFFFFu);
    (void)e1000_read32(E1000_REG_ICR);

    e1000_write32(E1000_REG_CTRL, e1000_read32(E1000_REG_CTRL) | E1000_CTRL_SLU);

    /* ---- RX ring ---- */
    e1000_rx_cur = 0;
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        e1000_rx_ring[i].addr_lo = (u32)e1000_rx_buf[i];
        e1000_rx_ring[i].addr_hi = 0;
        e1000_rx_ring[i].status = 0;
    }
    e1000_write32(E1000_REG_RDBAL, (u32)e1000_rx_ring);
    e1000_write32(E1000_REG_RDBAH, 0);
    e1000_write32(E1000_REG_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_write32(E1000_REG_RDH, 0);
    /* RDT = last index -> hands the hardware every descriptor except the
     * one right after RDH, the standard "leave one gap" ring convention
     * so head and tail are never ambiguously equal-and-full vs
     * equal-and-empty. */
    e1000_write32(E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
    e1000_write32(E1000_REG_RCTL,
        E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_BAM | E1000_RCTL_SECRC | E1000_RCTL_BSIZE_2048);

    /* ---- TX ring ---- */
    e1000_tx_cur = 0;
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        e1000_tx_ring[i].addr_lo = (u32)e1000_tx_buf[i];
        e1000_tx_ring[i].addr_hi = 0;
        e1000_tx_ring[i].status = E1000_TXD_STAT_DD; /* mark all slots "already done"/free to use */
    }
    e1000_write32(E1000_REG_TDBAL, (u32)e1000_tx_ring);
    e1000_write32(E1000_REG_TDBAH, 0);
    e1000_write32(E1000_REG_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_write32(E1000_REG_TDH, 0);
    e1000_write32(E1000_REG_TDT, 0);
    e1000_write32(E1000_REG_TCTL,
        E1000_TCTL_EN | E1000_TCTL_PSP
        | (0x0Fu << E1000_TCTL_CT_SHIFT)      /* collision threshold, per Intel's recommended default */
        | (0x40u << E1000_TCTL_COLD_SHIFT));  /* collision distance, full-duplex default */
    e1000_write32(E1000_REG_TIPG, 0x0060200A); /* Intel-recommended IPG timings for full duplex */

    /* QEMU's e1000 model pre-programs RAL0/RAH0 with the MAC it assigned
     * (same convention RTL8139 uses for its IDR registers) -- reading
     * those directly is simpler and just as correct as bit-banging an
     * EEPROM read for a card that already has the answer sitting in a
     * register. RAH0 bit 31 is "address valid"; we trust it's set since
     * QEMU always sets it, but there's nothing useful to do if it isn't
     * anyway. */
    u32 ral = e1000_read32(E1000_REG_RAL0);
    u32 rah = e1000_read32(E1000_REG_RAH0);
    nic.mac[0] = (u8)(ral & 0xFF);
    nic.mac[1] = (u8)((ral >> 8) & 0xFF);
    nic.mac[2] = (u8)((ral >> 16) & 0xFF);
    nic.mac[3] = (u8)((ral >> 24) & 0xFF);
    nic.mac[4] = (u8)(rah & 0xFF);
    nic.mac[5] = (u8)((rah >> 8) & 0xFF);

    nic.present = 1;
    nic.chip_name = "e1000";
    nic.send = e1000_send;
    nic.recv = e1000_recv;

    serial_puts("[e1000] initialized, MAC=");
    for (int i = 0; i < 6; i++) {
        serial_put_hex8(nic.mac[i]);
        if (i < 5) serial_putc(':');
    }
    serial_putc('\n');
    return 1;
}

#endif
