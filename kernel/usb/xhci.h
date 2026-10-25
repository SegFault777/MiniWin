/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef USB_XHCI_H
#define USB_XHCI_H
#include "usb.h"

/* usb/xhci.h -- the xHCI (USB 3.x) host controller driver: PCI class 0C:03, prog-if 0x30. The one every modern machine and
 * hypervisor has (QEMU qemu-xhci, VirtualBox "USB 3.0", VMware "USB 3.1"). It drives USB 1.1/2.0/3.x devices through the
 * same ports, so unlike EHCI there is no companion controller to hand anything to.
 *
 * What it does: reset and run the controller, take it over from the BIOS, hand it a command ring and an event ring, then
 * per device: Enable Slot, Address Device (which also performs SET_ADDRESS), control transfers on endpoint 0, Configure
 * Endpoint for the one interrupt-IN endpoint HID needs, and a Normal TRB that is re-armed every time it completes.
 * Everything is polled: the event ring is simply read again whenever somebody wants to know.
 *
 * The 64 KB DMA slice (MW_USB_DMA_ADDR + n * MW_USB_HC_STRIDE), all offsets from its start:
 *      0x0000 DCBAA (9 entries)   0x0100 scratchpad pointer array   0x0200 command ring (64 TRBs)
 *      0x0600 event ring (64 TRBs)   0x0A00 event ring segment table
 *      0x1000 * slot (slot 1..6): +0x000 output context, +0x600 input context, +0xC00 EP0 ring (16 TRBs),
 *                                 +0xD00 interrupt ring (16 TRBs), +0xE00 report buffer
 *      0x8000 scratchpad pages (up to 8 x 4 KB -- plenty for QEMU and the controllers we know; more fails the init, loudly) */

#define XHCI_MAX_SLOTS   6
#define XHCI_EP0_N       16
#define XHCI_INT_N       16

typedef struct { u32 base; u8 n, enq, cyc; } xhci_ring_t;

typedef struct {
    u32 op, rt, db;                   /* operational / runtime / doorbell register blocks */
    u8  cs;                           /* context size in bytes: 32, or 64 when HCCPARAMS1.CSZ is set */
    u8  maxslots;
    xhci_ring_t cmd;
    u32 ev_base; u8 ev_deq, ev_cyc;
    volatile u8 cmd_done, cmd_code, cmd_slot;
    xhci_ring_t ep0[XHCI_MAX_SLOTS + 1], intr[XHCI_MAX_SLOTS + 1];
    volatile u8 ep0_done[XHCI_MAX_SLOTS + 1], ep0_code[XHCI_MAX_SLOTS + 1];
    volatile u8 xf_done[XHCI_MAX_SLOTS + 1], xf_code[XHCI_MAX_SLOTS + 1];
    u32 xf_resid[XHCI_MAX_SLOTS + 1];
    u8  dci[XHCI_MAX_SLOTS + 1];
} xhci_t;
static xhci_t xhci_st[MW_USB_HC_MAX];

static inline u32 xhci_slot_base(usb_hc_t *hc, int slot) { return hc->dma + 0x1000u * (u32)slot; }

/* ---- rings ---- */
static void xhci_ring_init(xhci_ring_t *r, u32 base, u8 n) {
    usb_zero((void *)base, (u32)n * 16);
    r->base = base; r->n = n; r->enq = 0; r->cyc = 1;
}
/* Queues one TRB. The control word (with the cycle bit) is written LAST: that write is what hands the TRB to the
 * controller. When the next slot is the ring's last, a Link TRB back to the start goes in and the producer cycle flips. */
static void xhci_push(xhci_ring_t *r, u32 p0, u32 p1, u32 st, u32 ctl) {
    u32 t = r->base + (u32)r->enq * 16;
    usb_wr32(t, p0); usb_wr32(t + 4, p1); usb_wr32(t + 8, st);
    USB_BARRIER();
    usb_wr32(t + 12, (ctl & ~1u) | r->cyc);
    r->enq++;
    if (r->enq == r->n - 1) {
        u32 l = r->base + (u32)r->enq * 16;
        usb_wr32(l, r->base); usb_wr32(l + 4, 0); usb_wr32(l + 8, 0);
        USB_BARRIER();
        usb_wr32(l + 12, (6u << 10) | 2u | r->cyc);       /* Link TRB, Toggle Cycle */
        r->enq = 0; r->cyc ^= 1;
    }
}

/* ---- events ---- */
static void xhci_events(usb_hc_t *hc) {
    xhci_t *x = &xhci_st[hc->idx];
    int any = 0;
    for (;;) {
        u32 t = x->ev_base + (u32)x->ev_deq * 16;
        u32 ctl = usb_rd32(t + 12);
        if ((ctl & 1u) != x->ev_cyc) break;
        USB_BARRIER();
        u32 st = usb_rd32(t + 8);
        u32 type = (ctl >> 10) & 63u;
        if (type == 33) {                                   /* command completion */
            x->cmd_code = (u8)(st >> 24); x->cmd_slot = (u8)(ctl >> 24); x->cmd_done = 1;
        } else if (type == 32) {                            /* transfer event */
            u32 slot = ctl >> 24, ep = (ctl >> 16) & 31u;
            if (slot <= XHCI_MAX_SLOTS) {
                if (ep == 1) { x->ep0_code[slot] = (u8)(st >> 24); x->ep0_done[slot] = 1; }
                else { x->xf_code[slot] = (u8)(st >> 24); x->xf_resid[slot] = st & 0xFFFFFFu; x->xf_done[slot] = 1; }
            }
        }
        x->ev_deq++;
        if (x->ev_deq == 64) { x->ev_deq = 0; x->ev_cyc ^= 1; }
        any = 1;
    }
    if (any) {                                              /* tell the controller how far we have read (EHB = bit 3, write-1-to-clear) */
        u32 erdp = x->ev_base + (u32)x->ev_deq * 16;
        usb_wr32(x->rt + 0x38, erdp | 8u); usb_wr32(x->rt + 0x3C, 0);
    }
}

static int xhci_cmd(usb_hc_t *hc, u32 p0, u32 p1, u32 st, u32 ctl, u32 *slot_out) {
    xhci_t *x = &xhci_st[hc->idx];
    x->cmd_done = 0;
    xhci_push(&x->cmd, p0, p1, st, ctl);
    usb_wr32(x->db, 0);
    for (int i = 0; i < 1000; i++) {
        xhci_events(hc);
        if (x->cmd_done) { if (slot_out) *slot_out = x->cmd_slot; return x->cmd_code; }   /* 1 = success */
        delay_ms(1);
    }
    return -1;
}

/* ---- bringing the controller up ---- */
static int xhci_wait(u32 reg, u32 mask, u32 want, int ms) {
    for (int i = 0; i < ms; i++) {
        if ((usb_rd32(reg) & mask) == want) return 1;
        delay_ms(1);
    }
    return (usb_rd32(reg) & mask) == want;
}

static int xhci_init(usb_hc_t *hc, pci_device_t *pd) {
    xhci_t *x = &xhci_st[hc->idx];
    u32 bar0 = pd->bar0;
    if ((bar0 & 1u) || ((bar0 >> 1) & 3u) == 2u) {                   /* I/O BAR (never), or a 64-bit BAR: must sit below 4 GB */
        if ((bar0 & 1u) || pd->bar1 != 0) { usb_log("xHCI: register BAR is not below 4 GB, skipped"); return 0; }
    }
    hc->mmio = bar0 & ~0xFu;
    if (hc->mmio == 0) return 0;
    pci_enable_bus_mastering(pd->bus, pd->slot, pd->func);
    u32 m = hc->mmio;
    u32 caplen = usb_rd8(m);
    u32 hcs1 = usb_rd32(m + 4), hcs2 = usb_rd32(m + 8), hcc1 = usb_rd32(m + 0x10);
    x->op = m + caplen;
    x->db = m + (usb_rd32(m + 0x14) & ~3u);
    x->rt = m + (usb_rd32(m + 0x18) & ~0x1Fu);
    x->cs = (hcc1 & (1u << 2)) ? 64 : 32;
    hc->nports = (u8)(hcs1 >> 24);
    u32 maxslots = hcs1 & 0xFF;
    x->maxslots = (u8)(maxslots > XHCI_MAX_SLOTS ? XHCI_MAX_SLOTS : maxslots);

    /* BIOS hand-off: the extended capability list may hold a "USB Legacy Support" entry; claim the OS-owned bit and wait
     * for the firmware to let go (it would otherwise keep fiddling with the controller through SMIs). */
    u32 xecp = (hcc1 >> 16) & 0xFFFFu;
    for (int guard = 0; xecp && guard < 32; guard++) {
        u32 a = m + xecp * 4, cap = usb_rd32(a);
        if ((cap & 0xFF) == 1) {
            if (cap & (1u << 16)) {
                usb_wr32(a, cap | (1u << 24));
                for (int i = 0; i < 1000 && (usb_rd32(a) & (1u << 16)); i++) delay_ms(1);
            }
            usb_wr32(a + 4, 0xE0000000u);                           /* SMI enables off, stale status cleared */
        }
        u32 next = (cap >> 8) & 0xFF;
        if (!next) break;
        xecp += next;
    }

    /* stop, then reset */
    u32 cmd = usb_rd32(x->op);
    if (cmd & 1u) { usb_wr32(x->op, cmd & ~1u); xhci_wait(x->op + 4, 1u, 1u, 200); }
    usb_wr32(x->op, 2u);
    if (!xhci_wait(x->op, 2u, 0, 1000) || !xhci_wait(x->op + 4, 1u << 11, 0, 1000)) { usb_log("xHCI: reset did not finish"); return 0; }

    usb_zero((void *)hc->dma, 0x1000);
    u32 dcbaa = hc->dma, sp_arr = hc->dma + 0x100;
    u32 sp = (((hcs2 >> 21) & 0x1Fu) << 5) | ((hcs2 >> 27) & 0x1Fu);
    if (sp > 8) { usb_log_hex("xHCI: needs more scratchpad pages than the driver has: ", sp); return 0; }
    for (u32 i = 0; i < sp; i++) {
        u32 pg = hc->dma + 0x8000u + i * 0x1000u;
        usb_zero((void *)pg, 0x1000);
        usb_wr32(sp_arr + i * 8, pg); usb_wr32(sp_arr + i * 8 + 4, 0);
    }
    if (sp) { usb_wr32(dcbaa, sp_arr); usb_wr32(dcbaa + 4, 0); }

    xhci_ring_init(&x->cmd, hc->dma + 0x200, 64);
    x->ev_base = hc->dma + 0x600; usb_zero((void *)x->ev_base, 64 * 16);
    x->ev_deq = 0; x->ev_cyc = 1;
    u32 erst = hc->dma + 0xA00;
    usb_wr32(erst, x->ev_base); usb_wr32(erst + 4, 0); usb_wr32(erst + 8, 64); usb_wr32(erst + 12, 0);

    usb_wr32(x->op + 0x38, x->maxslots);                            /* CONFIG: slots enabled */
    usb_wr32(x->op + 0x30, dcbaa); usb_wr32(x->op + 0x34, 0);       /* DCBAAP */
    usb_wr32(x->op + 0x18, x->cmd.base | 1u); usb_wr32(x->op + 0x1C, 0);   /* CRCR, ring cycle state 1 */
    usb_wr32(x->rt + 0x28, 1);                                      /* ERSTSZ */
    usb_wr32(x->rt + 0x38, x->ev_base); usb_wr32(x->rt + 0x3C, 0); /* ERDP */
    usb_wr32(x->rt + 0x30, erst); usb_wr32(x->rt + 0x34, 0);        /* ERSTBA (last: it arms the ring) */
    usb_wr32(x->op, 1u);                                            /* Run */
    if (!xhci_wait(x->op + 4, 1u, 0, 500)) { usb_log("xHCI: would not leave the halted state"); return 0; }

    /* power every port that is not powered (some controllers have per-port power switches) */
    for (int p = 0; p < hc->nports; p++) {
        u32 pa = x->op + 0x400u + 0x10u * (u32)p, ps = usb_rd32(pa);
        if (!(ps & (1u << 9))) usb_wr32(pa, (ps & 0x0E00C200u) | (1u << 9));
    }
    delay_ms(100);                                                  /* let connect status settle */
    return 1;
}

/* One port's state change, for core.h: 0 = nothing new, 1 = a device is connected and the port is enabled (*speed set),
 * 2 = a device went away. `first` treats an already-connected port as new. */
static int xhci_port_scan(usb_hc_t *hc, int port, int first, int *speed) {
    xhci_t *x = &xhci_st[hc->idx];
    u32 pa = x->op + 0x400u + 0x10u * (u32)port, ps = usb_rd32(pa);
    u32 ch = ps & 0x00FE0000u;                                      /* the write-1-to-clear change bits */
    if (ch) usb_wr32(pa, (ps & 0x0E00C200u) | ch);
    if (!(ch & (1u << 17)) && !first) return 0;
    if (!(ps & 1u)) return (ch & (1u << 17)) ? 2 : 0;
    if (!(ps & 2u)) {                                               /* not enabled yet: USB 2 ports need a reset */
        usb_wr32(pa, (usb_rd32(pa) & 0x0E00C200u) | (1u << 4));
        for (int i = 0; i < 500; i++) { if (usb_rd32(pa) & (1u << 21)) break; delay_ms(1); }
        ps = usb_rd32(pa);
        usb_wr32(pa, (ps & 0x0E00C200u) | (ps & 0x00FE0000u));
        delay_ms(10);
        ps = usb_rd32(pa);
        if (!(ps & 2u)) return 0;
    }
    *speed = (int)((ps >> 10) & 15u);
    return 1;
}

/* ---- per device ---- */
static int xhci_address(usb_hc_t *hc, usb_dev_t *d) {
    xhci_t *x = &xhci_st[hc->idx];
    u32 slot = 0;
    if (xhci_cmd(hc, 0, 0, 0, 9u << 10, &slot) != 1 || slot == 0 || slot > XHCI_MAX_SLOTS) return 0;   /* Enable Slot */
    d->slot = (u8)slot;
    u32 base = xhci_slot_base(hc, (int)slot), cs = x->cs;
    usb_zero((void *)base, 0x1000);
    u32 out = base, in = base + 0x600;
    usb_wr32(hc->dma + 8u * slot, out); usb_wr32(hc->dma + 8u * slot + 4, 0);                  /* DCBAA[slot] */
    xhci_ring_init(&x->ep0[slot], base + 0xC00, XHCI_EP0_N);
    u32 mps = d->speed == USB_SPEED_SUPER ? 512 : d->speed == USB_SPEED_HIGH ? 64 : 8;
    d->mps0 = (u16)mps;
    usb_wr32(in + 4, 3u);                                           /* Add: slot context + EP0 */
    usb_wr32(in + cs, ((u32)d->speed << 20) | (1u << 27));          /* slot: speed, one context entry */
    usb_wr32(in + cs + 4, ((u32)d->port + 1u) << 16);               /* slot: root hub port number (1-based) */
    usb_wr32(in + 2 * cs, 0);
    usb_wr32(in + 2 * cs + 4, (3u << 1) | (4u << 3) | (mps << 16));/* EP0: CErr 3, control, max packet */
    usb_wr32(in + 2 * cs + 8, x->ep0[slot].base | 1u); usb_wr32(in + 2 * cs + 12, 0);
    usb_wr32(in + 2 * cs + 16, 8);
    if (xhci_cmd(hc, in, 0, 0, (11u << 10) | (slot << 24), 0) != 1) {                           /* Address Device */
        usb_log("xHCI: Address Device failed"); return 0;
    }
    return 1;
}

static void xhci_set_mps0(usb_hc_t *hc, usb_dev_t *d, u32 mps) {
    xhci_t *x = &xhci_st[hc->idx];
    u32 base = xhci_slot_base(hc, d->slot), in = base + 0x600, cs = x->cs;
    usb_zero((void *)in, 0x600);
    usb_wr32(in + 4, 2u);                                           /* Add: EP0 only */
    usb_wr32(in + 2 * cs + 4, (3u << 1) | (4u << 3) | (mps << 16));
    xhci_cmd(hc, in, 0, 0, (13u << 10) | ((u32)d->slot << 24), 0);  /* Evaluate Context */
    d->mps0 = (u16)mps;
}

/* Control transfer on EP0: returns bytes moved in the data stage (0 for none), or -1. */
static int xhci_control(usb_hc_t *hc, usb_dev_t *d, const u8 *setup, u16 len, int in) {
    xhci_t *x = &xhci_st[hc->idx];
    u32 s = d->slot;
    xhci_ring_t *r = &x->ep0[s];
    u32 s0 = (u32)setup[0] | ((u32)setup[1] << 8) | ((u32)setup[2] << 16) | ((u32)setup[3] << 24);
    u32 s1 = (u32)setup[4] | ((u32)setup[5] << 8) | ((u32)setup[6] << 16) | ((u32)setup[7] << 24);
    u32 trt = len == 0 ? 0u : (in ? 3u : 2u);
    x->ep0_done[s] = 0;
    xhci_push(r, s0, s1, 8, (2u << 10) | (1u << 6) | (trt << 16));                    /* Setup Stage, immediate data */
    if (len) xhci_push(r, (u32)usb_cbuf, 0, len, (3u << 10) | (in ? (1u << 16) : 0u)); /* Data Stage */
    xhci_push(r, 0, 0, 0, (4u << 10) | (1u << 5) | ((len && in) ? 0u : (1u << 16)));   /* Status Stage, interrupt on completion */
    usb_wr32(x->db + 4u * s, 1);
    for (int i = 0; i < 1000; i++) {
        xhci_events(hc);
        if (x->ep0_done[s]) {
            u8 c = x->ep0_code[s];
            return (c == 1 || c == 13) ? (int)len : -1;
        }
        delay_ms(1);
    }
    return -1;
}

static void xhci_arm(usb_hc_t *hc, usb_dev_t *d) {
    xhci_t *x = &xhci_st[hc->idx];
    u32 s = d->slot, rbuf = xhci_slot_base(hc, (int)s) + 0xE00;
    xhci_push(&x->intr[s], rbuf, 0, d->rlen, (1u << 10) | (1u << 5) | (1u << 2));       /* Normal TRB: IOC, short packet OK */
    usb_wr32(x->db + 4u * s, x->dci[s]);
}

static int xhci_setup_intr(usb_hc_t *hc, usb_dev_t *d) {
    xhci_t *x = &xhci_st[hc->idx];
    u32 s = d->slot, base = xhci_slot_base(hc, (int)s), in = base + 0x600, out = base, cs = x->cs;
    if (d->ep < 1 || d->ep > 7) return 0;
    u32 dci = (u32)d->ep * 2u + 1u;
    x->dci[s] = (u8)dci;
    xhci_ring_init(&x->intr[s], base + 0xD00, XHCI_INT_N);
    usb_zero((void *)in, 0x600);
    usb_wr32(in + 4, 1u | (1u << dci));
    for (u32 i = 0; i < 32; i += 4) usb_wr32(in + cs + i, usb_rd32(out + i));        /* keep the slot context as it is ... */
    usb_wr32(in + cs, (usb_rd32(in + cs) & ~(31u << 27)) | (dci << 27));              /* ... except the entry count */
    u32 ep = in + cs * (dci + 1u);
    u32 iv;                                                          /* interval, in the xHCI's 2^n x 125 us units */
    if (d->speed == USB_SPEED_HIGH || d->speed == USB_SPEED_SUPER) { iv = d->ep_interval ? (u32)d->ep_interval - 1u : 0; }
    else { u32 ms = d->ep_interval ? d->ep_interval : 1u; iv = 3; while ((1u << (iv - 3)) < ms && iv < 10) iv++; }
    if (iv > 15) iv = 15;
    usb_wr32(ep, iv << 16);
    usb_wr32(ep + 4, (3u << 1) | (7u << 3) | ((u32)d->ep_mps << 16));               /* CErr 3, Interrupt IN */
    usb_wr32(ep + 8, x->intr[s].base | 1u); usb_wr32(ep + 12, 0);
    usb_wr32(ep + 16, (u32)d->ep_mps | ((u32)d->ep_mps << 16));
    if (xhci_cmd(hc, in, 0, 0, (12u << 10) | (s << 24), 0) != 1) { usb_log("xHCI: Configure Endpoint failed"); return 0; }
    xhci_arm(hc, d);
    return 1;
}

/* After core.h has run xhci_events(): did the interrupt transfer finish? Copies the report to `buf` and re-arms. */
static int xhci_intr_poll(usb_hc_t *hc, usb_dev_t *d, u8 *buf) {
    xhci_t *x = &xhci_st[hc->idx];
    u32 s = d->slot;
    if (!x->xf_done[s]) return -1;
    x->xf_done[s] = 0;
    u8 c = x->xf_code[s];
    if (c != 1 && c != 13) { d->dead = 1; return -1; }
    u32 got = d->rlen > x->xf_resid[s] ? d->rlen - x->xf_resid[s] : 0;
    u32 rbuf = xhci_slot_base(hc, (int)s) + 0xE00;
    for (u32 i = 0; i < got && i < 64; i++) buf[i] = *(volatile u8 *)(rbuf + i);
    xhci_arm(hc, d);
    return (int)got;
}

static void xhci_release(usb_hc_t *hc, usb_dev_t *d) {            /* the device left: give its slot back */
    if (d->slot) xhci_cmd(hc, 0, 0, 0, (10u << 10) | ((u32)d->slot << 24), 0);
}
#endif
