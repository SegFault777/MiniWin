/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef USB_OHCI_H
#define USB_OHCI_H
#include "usb.h"

/* usb/ohci.h -- the OHCI (USB 1.1) host controller driver: PCI class 0C:03, prog-if 0x10. VirtualBox's "USB 1.1" controller,
 * QEMU's pci-ohci, the AMD/NEC/Apple chipsets' real ones -- and the COMPANION that EHCI hands every low- and full-speed
 * device to, which is why this driver is the one that actually finds most real keyboards and mice.
 *
 * OHCI is built from endpoint descriptors (ED) pointing at chains of transfer descriptors (TD). A control transfer builds
 * setup/data/status TDs behind one ED on the control list, starts the list and waits for the ED's head pointer to reach
 * its tail; an interrupt endpoint is an ED hung in every slot of the periodic table (HCCA) whose tail TD is renewed after
 * each completion. The controller turns finished TDs into a "done queue" it announces by interrupt; we do not use
 * interrupts, but the controller will not report further completions until that announcement is acknowledged, so
 * ohci_events() does exactly that.
 *
 * The 64 KB DMA slice, offsets from its start:
 *      0x0000 HCCA (256 bytes, 256-aligned)    0x0100 control ED    0x0120 four control TDs (setup, data, status, tail)
 *      0x0160 the 8 setup bytes                0x0170 periodic anchor ED (K bit set: always skipped)
 *      0x0200 + 0x100 * n   interrupt device n (0..3): ED +0x00, TD A +0x10, TD B +0x20, report buffer +0x40 */

#define OHCI_MAX_INT 4

typedef struct {
    u32 base;
    u8  used[OHCI_MAX_INT];
    u8  cur[OHCI_MAX_INT];           /* which of the device's two TDs is the live one (the other is the dummy tail) */
} ohci_t;
static ohci_t ohci_st[MW_USB_HC_MAX];

#define OHCI_HCCA     0x0000u
#define OHCI_CED      0x0100u
#define OHCI_CTD      0x0120u
#define OHCI_SETUP    0x0160u
#define OHCI_ANCHOR   0x0170u
#define OHCI_INT_BASE 0x0200u

static int ohci_init(usb_hc_t *hc, pci_device_t *pd) {
    ohci_t *o = &ohci_st[hc->idx];
    if (pd->bar0 & 1u) return 0;
    hc->mmio = pd->bar0 & ~0xFu;
    if (!hc->mmio) return 0;
    pci_enable_bus_mastering(pd->bus, pd->slot, pd->func);
    u32 m = hc->mmio;
    o->base = m;
    if ((usb_rd32(m) & 0xFFu) != 0x10u) { usb_log_hex("OHCI: unexpected revision ", usb_rd32(m)); return 0; }

    u32 ctrl = usb_rd32(m + 4);
    if (ctrl & (1u << 8)) {                                          /* firmware (SMM) owns it: ask for it back */
        usb_wr32(m + 8, 1u << 3);
        for (int i = 0; i < 500 && (usb_rd32(m + 4) & (1u << 8)); i++) delay_ms(1);
    }
    u32 fi = usb_rd32(m + 0x34);
    usb_wr32(m + 0x14, 0xC0000000u);                                 /* every interrupt source off */
    usb_wr32(m + 8, 1u);                                             /* HostControllerReset */
    for (int i = 0; i < 30 && (usb_rd32(m + 8) & 1u); i++) delay_ms(1);
    if (usb_rd32(m + 8) & 1u) { usb_log("OHCI: reset did not finish"); return 0; }

    usb_zero((void *)hc->dma, 0x1000);
    u32 anchor = hc->dma + OHCI_ANCHOR;
    usb_wr32(anchor, 1u << 14);                                      /* ED with the skip bit: never serviced */
    for (u32 i = 0; i < 32; i++) usb_wr32(hc->dma + OHCI_HCCA + i * 4, anchor);

    u32 fi14 = fi & 0x3FFFu;
    if (fi14 < 0x2000u) fi14 = 0x2EDFu;                              /* the spec's default: 11999 bit times */
    usb_wr32(m + 0x34, fi14 | ((((fi14 - 210u) * 6u) / 7u) << 16) | ((fi ^ 0x80000000u) & 0x80000000u));
    usb_wr32(m + 0x40, (fi14 * 9u) / 10u);                           /* PeriodicStart */
    usb_wr32(m + 0x44, 0x628u);                                      /* LSThreshold */
    usb_wr32(m + 0x18, hc->dma + OHCI_HCCA);
    usb_wr32(m + 0x20, 0); usb_wr32(m + 0x28, 0);
    usb_wr32(m + 0x0C, 0xFFFFFFFFu);
    usb_wr32(m + 4, (2u << 6) | (1u << 2));                          /* Operational, periodic list on */

    u32 da = usb_rd32(m + 0x48);
    hc->nports = (u8)(da & 0xFFu);
    if (hc->nports > 15) hc->nports = 15;
    usb_wr32(m + 0x50, 1u << 16);                                    /* SetGlobalPower */
    for (int p = 0; p < hc->nports; p++) usb_wr32(m + 0x54u + 4u * (u32)p, 1u << 8);
    u32 pw = ((da >> 24) & 0xFFu) * 2u;
    delay_ms(pw > 100 ? 100 : (pw < 20 ? 20 : pw));
    return 1;
}

static void ohci_events(usb_hc_t *hc) {                              /* acknowledge the done queue so completions keep flowing */
    /* The controller writes the done-queue head into the HCCA (plain memory) before it raises WDH, so looking there costs
     * nothing -- a register read is a VM exit on a hypervisor, and this runs every pass of the main loop. */
    if (usb_rd32(hc->dma + OHCI_HCCA + 0x84)) {
        usb_wr32(hc->dma + OHCI_HCCA + 0x84, 0);
        usb_wr32(hc->mmio + 0x0C, 2u);
    }
}

static int ohci_port_scan(usb_hc_t *hc, int port, int first, int *speed) {
    u32 m = hc->mmio, reg = m + 0x54u + 4u * (u32)port, ps = usb_rd32(reg);
    u32 csc = ps & (1u << 16);
    if (csc) usb_wr32(reg, csc);
    if (!csc && !first) return 0;
    if (!(ps & 1u)) return csc ? 2 : 0;
    usb_wr32(reg, 1u << 4);                                          /* PortReset */
    for (int i = 0; i < 200; i++) { if (usb_rd32(reg) & (1u << 20)) break; delay_ms(1); }
    usb_wr32(reg, 1u << 20);
    delay_ms(10);
    ps = usb_rd32(reg);
    if (!(ps & 2u)) { usb_wr32(reg, 1u << 1); delay_ms(5); ps = usb_rd32(reg); }
    if (!(ps & 1u)) return 0;
    *speed = (ps & (1u << 9)) ? USB_SPEED_LOW : USB_SPEED_FULL;
    return 1;
}

static int ohci_address(usb_hc_t *hc, usb_dev_t *d) {
    (void)hc;
    d->mps0 = 8;
    return 1;                                                        /* SET_ADDRESS is core.h's job */
}

static void ohci_td(u32 td, u32 dp, u32 toggle, u32 round, u32 buf, u32 len, u32 next) {
    usb_wr32(td, (round << 18) | (dp << 19) | (7u << 21) | (toggle << 24) | (0xFu << 28));
    usb_wr32(td + 4, len ? buf : 0);
    usb_wr32(td + 8, next);
    usb_wr32(td + 12, len ? buf + len - 1 : 0);
}

static int ohci_control(usb_hc_t *hc, usb_dev_t *d, const u8 *setup, u16 len, int in) {
    u32 m = hc->mmio, ed = hc->dma + OHCI_CED, t0 = hc->dma + OHCI_CTD, t1 = t0 + 16, t2 = t1 + 16, t3 = t2 + 16, sb = hc->dma + OHCI_SETUP;
    u32 ctrl = usb_rd32(m + 4);
    usb_wr32(m + 4, ctrl & ~(1u << 4));                              /* control list off while we rebuild it */
    delay_ms(2);
    for (int i = 0; i < 8; i++) *(volatile u8 *)(sb + (u32)i) = setup[i];
    ohci_td(t0, 0, 2, 0, sb, 8, len ? t1 : t2);                       /* SETUP, DATA0 */
    if (len) ohci_td(t1, in ? 2u : 1u, 3, in ? 1u : 0u, (u32)usb_cbuf, len, t2);   /* DATA, DATA1 (short IN packets allowed) */
    ohci_td(t2, (in && len) ? 1u : 2u, 3, 0, 0, 0, t3);               /* STATUS, DATA1, opposite direction */
    usb_zero((void *)t3, 16);
    usb_wr32(ed, (u32)d->addr | ((u32)(d->speed == USB_SPEED_LOW) << 13) | ((u32)d->mps0 << 16));
    usb_wr32(ed + 4, t3);                                            /* TailP */
    usb_wr32(ed + 8, t0);                                            /* HeadP */
    usb_wr32(ed + 12, 0);
    usb_wr32(m + 0x20, ed); usb_wr32(m + 0x24, 0);
    usb_wr32(m + 4, ctrl | (1u << 4));
    usb_wr32(m + 8, 1u << 1);                                        /* ControlListFilled */
    int res = -1;
    for (int i = 0; i < 1000; i++) {
        u32 head = usb_rd32(ed + 8);
        if (head & 1u) break;                                        /* halted: a TD failed */
        if ((head & ~0xFu) == t3) {
            u32 c0 = usb_rd32(t0) >> 28, c2 = usb_rd32(t2) >> 28, c1 = len ? (usb_rd32(t1) >> 28) : 0;
            if (c0 == 0 && c1 == 0 && c2 == 0) {
                u32 left = 0;
                if (len) { u32 cbp = usb_rd32(t1 + 4), be = usb_rd32(t1 + 12); left = cbp ? be + 1 - cbp : 0; }
                res = (int)(len - left);
            }
            break;
        }
        delay_ms(1);
    }
    usb_wr32(m + 4, usb_rd32(m + 4) & ~(1u << 4));
    ohci_events(hc);
    return res;
}

static int ohci_setup_intr(usb_hc_t *hc, usb_dev_t *d) {
    ohci_t *o = &ohci_st[hc->idx];
    int n = -1;
    for (int i = 0; i < OHCI_MAX_INT; i++) if (!o->used[i]) { n = i; break; }
    if (n < 0) return 0;
    o->used[n] = 1; o->cur[n] = 0; d->slot = (u8)n;
    u32 base = hc->dma + OHCI_INT_BASE + 0x100u * (u32)n, ed = base, ta = base + 0x10, tb = base + 0x20, rb = base + 0x40;
    usb_zero((void *)base, 0x100);
    ohci_td(ta, 2, 0, 1, rb, d->rlen, tb);                           /* IN, toggle from the ED's carry bit, short packets OK */
    usb_wr32(ed, (u32)d->addr | ((u32)d->ep << 7) | ((u32)(d->speed == USB_SPEED_LOW) << 13) | ((u32)d->ep_mps << 16));
    usb_wr32(ed + 4, tb);
    usb_wr32(ed + 8, ta);
    u32 anchor = hc->dma + OHCI_ANCHOR;
    usb_wr32(ed + 12, usb_rd32(anchor + 12));
    USB_BARRIER();
    usb_wr32(anchor + 12, ed);                                       /* hang it behind the anchor ED */
    return 1;
}

static int ohci_intr_poll(usb_hc_t *hc, usb_dev_t *d, u8 *buf) {
    ohci_t *o = &ohci_st[hc->idx];
    u32 n = d->slot;
    u32 base = hc->dma + OHCI_INT_BASE + 0x100u * n, ed = base, rb = base + 0x40;
    u32 live = base + (o->cur[n] ? 0x20u : 0x10u), spare = base + (o->cur[n] ? 0x10u : 0x20u);
    u32 head = usb_rd32(ed + 8);
    if ((head & ~0xFu) != spare) {
        if (head & 1u) d->dead = 1;                                  /* halted */
        return -1;                                                   /* the live TD has not completed yet */
    }
    u32 cc = usb_rd32(live) >> 28;
    int got = -1;
    if (cc == 0) {
        u32 cbp = usb_rd32(live + 4), be = usb_rd32(live + 12), want = d->rlen, left = cbp ? be + 1 - cbp : 0;
        got = (int)(want - left);
        for (int i = 0; i < got && i < 64; i++) buf[i] = *(volatile u8 *)(rb + (u32)i);
    } else d->dead = 1;
    /* the spare (dummy) TD becomes the live one; the finished one becomes the new dummy tail */
    ohci_td(spare, 2, 0, 1, rb, d->rlen, live);
    usb_zero((void *)live, 16);
    USB_BARRIER();
    usb_wr32(ed + 4, live);                                          /* TailP */
    o->cur[n] ^= 1;
    return got;
}

static void ohci_release(usb_hc_t *hc, usb_dev_t *d) {
    ohci_t *o = &ohci_st[hc->idx];
    u32 ed = hc->dma + OHCI_INT_BASE + 0x100u * (u32)d->slot;
    usb_wr32(ed, usb_rd32(ed) | (1u << 14));                         /* skip it from now on */
    o->used[d->slot] = 0;
}
#endif
