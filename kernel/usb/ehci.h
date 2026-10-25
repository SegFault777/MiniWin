/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef USB_EHCI_H
#define USB_EHCI_H
#include "usb.h"

/* usb/ehci.h -- the EHCI (USB 2.0) host controller driver: PCI class 0C:03, prog-if 0x20.
 *
 * An EHCI controller only speaks HIGH speed. Every low- and full-speed device (which is to say nearly every keyboard and
 * mouse that exists) is handed to the controller's COMPANION (OHCI or UHCI) by setting the port-owner bit -- the
 * companion then sees an ordinary connect. So this driver's real jobs are: take the controller over from the BIOS, reset
 * it, route the ports to it (CONFIGFLAG), give away what is not high speed, and run the high-speed devices that remain
 * (virtual tablets, and the odd real HID gadget). On a machine whose companions are UHCI -- which this kernel does not
 * drive -- low/full-speed devices behind EHCI therefore stay dark; that is logged.
 *
 * The 64 KB DMA slice, offsets from its start:
 *      0x0000 periodic frame list (1024 entries, 4 KB)          0x1000 control QH (a one-QH asynchronous ring)
 *      0x1040 three control qTDs (setup, data, status)          0x10E0 the 8 setup bytes
 *      0x1100 periodic anchor QH (every frame-list entry points at it; interrupt QHs hang off it)
 *      0x2000 + 0x100 * n   interrupt device n (0..3): QH +0x00, qTD +0x40, report buffer +0x80
 * A control transfer stops the asynchronous schedule, builds its three qTDs, starts it again and polls the last qTD: slow
 * in principle, simple and race-free in practice, and it only runs while enumerating. */

#define EHCI_MAX_INT 4

typedef struct {
    u32 op;
    u8  used[EHCI_MAX_INT];
    u8  dt[EHCI_MAX_INT];
} ehci_t;
static ehci_t ehci_st[MW_USB_HC_MAX];

#define EHCI_QH_CTL     0x1000u
#define EHCI_QTD_CTL    0x1040u
#define EHCI_SETUP_BUF  0x10E0u
#define EHCI_ANCHOR     0x1100u
#define EHCI_INT_BASE   0x2000u

static void ehci_zero_qtd(u32 q) { usb_zero((void *)q, 32); usb_wr32(q, 1); usb_wr32(q + 4, 1); }

static int ehci_wait(u32 reg, u32 mask, u32 want, int ms) {
    for (int i = 0; i < ms; i++) {
        if ((usb_rd32(reg) & mask) == want) return 1;
        delay_ms(1);
    }
    return (usb_rd32(reg) & mask) == want;
}

static int ehci_init(usb_hc_t *hc, pci_device_t *pd) {
    ehci_t *e = &ehci_st[hc->idx];
    if ((pd->bar0 & 1u) || ((pd->bar0 >> 1) & 3u) == 2u) { if ((pd->bar0 & 1u) || pd->bar1) { usb_log("EHCI: register BAR not below 4 GB, skipped"); return 0; } }
    hc->mmio = pd->bar0 & ~0xFu;
    if (!hc->mmio) return 0;
    pci_enable_bus_mastering(pd->bus, pd->slot, pd->func);
    u32 m = hc->mmio, caplen = usb_rd8(m);
    u32 hcs = usb_rd32(m + 4), hcc = usb_rd32(m + 8);
    e->op = m + caplen;
    hc->nports = (u8)(hcs & 0xF);

    /* BIOS hand-off through PCI config space (EECP): the legacy-support capability has a BIOS-owned and an OS-owned bit */
    u32 eecp = (hcc >> 8) & 0xFFu;
    for (int guard = 0; eecp >= 0x40 && guard < 16; guard++) {
        u32 cap = pci_config_read32(pd->bus, pd->slot, pd->func, (u8)eecp);
        if ((cap & 0xFF) == 1) {
            if (cap & (1u << 16)) {
                pci_config_write32(pd->bus, pd->slot, pd->func, (u8)eecp, cap | (1u << 24));
                for (int i = 0; i < 1000 && (pci_config_read32(pd->bus, pd->slot, pd->func, (u8)eecp) & (1u << 16)); i++) delay_ms(1);
            }
            pci_config_write32(pd->bus, pd->slot, pd->func, (u8)(eecp + 4), 0);
        }
        eecp = (cap >> 8) & 0xFFu;
    }

    u32 cmd = usb_rd32(e->op);
    if (cmd & 1u) { usb_wr32(e->op, cmd & ~1u); ehci_wait(e->op + 4, 1u << 12, 1u << 12, 200); }
    usb_wr32(e->op, 2u);
    if (!ehci_wait(e->op, 2u, 0, 500)) { usb_log("EHCI: reset did not finish"); return 0; }

    usb_zero((void *)hc->dma, 0x4000);
    u32 anchor = hc->dma + EHCI_ANCHOR;
    usb_wr32(anchor, 1u | 2u); usb_wr32(anchor + 4, (2u << 12)); usb_wr32(anchor + 8, 0);
    usb_wr32(anchor + 16, 1); usb_wr32(anchor + 20, 1); usb_wr32(anchor + 24, 0x40u);   /* a QH that is empty and halted: skipped */
    for (u32 i = 0; i < 1024; i++) usb_wr32(hc->dma + i * 4, anchor | 2u);
    u32 qh = hc->dma + EHCI_QH_CTL;
    usb_wr32(qh, qh | 2u);                                           /* the one-QH async ring points at itself */
    usb_wr32(qh + 4, (1u << 15) | (2u << 12));
    usb_wr32(qh + 16, 1); usb_wr32(qh + 20, 1);

    if (hcc & 1u) usb_wr32(e->op + 0x10, 0);                         /* 4 GB segment 0 */
    usb_wr32(e->op + 8, 0);                                          /* no interrupts */
    usb_wr32(e->op + 0x14, hc->dma);                                 /* PERIODICLISTBASE */
    usb_wr32(e->op + 0x18, qh);                                      /* ASYNCLISTADDR */
    usb_wr32(e->op + 4, 0x3Fu);                                      /* clear stale status */
    usb_wr32(e->op, (8u << 16) | (1u << 4) | 1u);                    /* Run, periodic schedule on, 1 ms interrupt threshold */
    if (!ehci_wait(e->op + 4, 1u << 12, 0, 500)) { usb_log("EHCI: would not start"); return 0; }
    usb_wr32(e->op + 0x40, 1u);                                      /* CONFIGFLAG: route all ports to us */
    for (int p = 0; p < hc->nports; p++) {
        u32 pa = e->op + 0x44u + 4u * (u32)p, ps = usb_rd32(pa);
        if (!(ps & (1u << 12))) usb_wr32(pa, ps | (1u << 12));
    }
    delay_ms(100);
    return 1;
}

/* see xhci_port_scan: 0 nothing, 1 a high-speed device is ready, 2 it left. Anything slower is handed to the companion. */
static int ehci_port_scan(usb_hc_t *hc, int port, int first, int *speed) {
    ehci_t *e = &ehci_st[hc->idx];
    u32 pa = e->op + 0x44u + 4u * (u32)port, ps = usb_rd32(pa);
    u32 csc = ps & 2u;
    if (csc) usb_wr32(pa, ps);                                       /* write-1-to-clear on the change bits, the rest rewritten as read */
    if (!csc && !first) return 0;
    if (ps & (1u << 13)) return 0;                                   /* the companion owns this port */
    if (!(ps & 1u)) return csc ? 2 : 0;
    if (((ps >> 10) & 3u) == 1u) {                                   /* K state: a low-speed device -- not ours */
        usb_wr32(pa, usb_rd32(pa) | (1u << 13));
        usb_log("EHCI: low-speed device handed to the companion controller");
        return 0;
    }
    usb_wr32(pa, (usb_rd32(pa) & ~4u) | (1u << 8));                  /* port reset */
    delay_ms(50);
    usb_wr32(pa, usb_rd32(pa) & ~(1u << 8));
    for (int i = 0; i < 20 && (usb_rd32(pa) & (1u << 8)); i++) delay_ms(1);
    delay_ms(10);
    ps = usb_rd32(pa);
    if (!(ps & 4u)) {                                                /* not enabled after reset: not high speed */
        usb_wr32(pa, ps | (1u << 13));
        usb_log("EHCI: full-speed device handed to the companion controller");
        return 0;
    }
    *speed = USB_SPEED_HIGH;
    return 1;
}

static int ehci_address(usb_hc_t *hc, usb_dev_t *d) {
    (void)hc;
    d->mps0 = 64;
    return 1;                                                        /* SET_ADDRESS itself is core.h's job (it needs control()) */
}

/* One control transfer. `addr` is the device's USB address (0 while it is still being addressed). */
static int ehci_control(usb_hc_t *hc, usb_dev_t *d, const u8 *setup, u16 len, int in) {
    ehci_t *e = &ehci_st[hc->idx];
    u32 qh = hc->dma + EHCI_QH_CTL, q0 = hc->dma + EHCI_QTD_CTL, q1 = q0 + 32, q2 = q1 + 32, sb = hc->dma + EHCI_SETUP_BUF;
    u32 cmd = usb_rd32(e->op);
    if (cmd & (1u << 5)) { usb_wr32(e->op, cmd & ~(1u << 5)); ehci_wait(e->op + 4, 1u << 15, 0, 100); }
    for (int i = 0; i < 8; i++) *(volatile u8 *)(sb + (u32)i) = setup[i];
    ehci_zero_qtd(q0); ehci_zero_qtd(q1); ehci_zero_qtd(q2);
    usb_wr32(q0 + 8, (2u << 8) | (3u << 10) | (8u << 16) | 0x80u);
    usb_wr32(q0 + 12, sb);
    u32 last = q0;
    if (len) {
        usb_wr32(q0, q1);
        usb_wr32(q1 + 8, (1u << 31) | ((in ? 1u : 0u) << 8) | (3u << 10) | ((u32)len << 16) | 0x80u);
        usb_wr32(q1 + 12, (u32)usb_cbuf);
        usb_wr32(q1, q2);
    } else usb_wr32(q0, q2);
    usb_wr32(q2 + 8, (1u << 31) | (((in && len) ? 0u : 1u) << 8) | (3u << 10) | (1u << 15) | 0x80u);
    last = q2;
    usb_wr32(qh + 4, (u32)d->addr | (2u << 12) | (1u << 14) | (1u << 15) | ((u32)d->mps0 << 16));
    usb_wr32(qh + 8, 1u << 30);
    usb_wr32(qh + 12, 0); usb_wr32(qh + 20, 1); usb_wr32(qh + 24, 0);
    usb_wr32(qh + 16, q0);                                           /* next qTD: this arms the QH */
    usb_wr32(e->op + 0x18, qh);
    usb_wr32(e->op, usb_rd32(e->op) | (1u << 5));
    int res = -1;
    for (int i = 0; i < 1000; i++) {
        u32 t0 = usb_rd32(q0 + 8), t1 = len ? usb_rd32(q1 + 8) : 0, t2 = usb_rd32(last + 8);
        if ((t0 | t1 | t2) & 0x78u) break;                           /* halted / buffer / babble / transaction error */
        if (!(t2 & 0x80u)) {
            res = len ? (int)(len - ((t1 >> 16) & 0x7FFFu)) : 0;
            break;
        }
        delay_ms(1);
    }
    cmd = usb_rd32(e->op);
    usb_wr32(e->op, cmd & ~(1u << 5));
    ehci_wait(e->op + 4, 1u << 15, 0, 100);
    return res;
}

static int ehci_setup_intr(usb_hc_t *hc, usb_dev_t *d) {
    ehci_t *e = &ehci_st[hc->idx];
    int n = -1;
    for (int i = 0; i < EHCI_MAX_INT; i++) if (!e->used[i]) { n = i; break; }
    if (n < 0) return 0;
    e->used[n] = 1; e->dt[n] = 0; d->slot = (u8)n;
    u32 base = hc->dma + EHCI_INT_BASE + 0x100u * (u32)n, qh = base, q = base + 0x40, rb = base + 0x80;
    usb_zero((void *)base, 0x100);
    usb_wr32(q, 1); usb_wr32(q + 4, 1);
    usb_wr32(q + 8, (1u << 8) | (3u << 10) | (1u << 15) | ((u32)d->rlen << 16) | 0x80u);
    usb_wr32(q + 12, rb);
    usb_wr32(qh + 4, (u32)d->addr | ((u32)d->ep << 8) | (2u << 12) | ((u32)d->ep_mps << 16));
    usb_wr32(qh + 8, (1u << 30) | 1u);                               /* one transaction, in micro-frame 0 */
    usb_wr32(qh + 20, 1);
    usb_wr32(qh + 16, q);
    u32 anchor = hc->dma + EHCI_ANCHOR;
    usb_wr32(qh, usb_rd32(anchor));                                  /* link it in right behind the anchor */
    USB_BARRIER();
    usb_wr32(anchor, qh | 2u);
    return 1;
}

static int ehci_intr_poll(usb_hc_t *hc, usb_dev_t *d, u8 *buf) {
    ehci_t *e = &ehci_st[hc->idx];
    u32 n = d->slot;
    u32 base = hc->dma + EHCI_INT_BASE + 0x100u * n, qh = base, q = base + 0x40, rb = base + 0x80;
    u32 tok = usb_rd32(q + 8);
    if (tok & 0x80u) return -1;                                      /* still waiting for the device */
    if (tok & 0x78u) { d->dead = 1; return -1; }
    u32 left = (tok >> 16) & 0x7FFFu;
    u32 got = d->rlen > left ? d->rlen - left : 0;
    for (u32 i = 0; i < got && i < 64; i++) buf[i] = *(volatile u8 *)(rb + i);
    e->dt[n] ^= 1;
    usb_wr32(q, 1); usb_wr32(q + 4, 1);
    usb_wr32(q + 12, rb);                                            /* the controller moves the buffer pointer as it fills it: a re-armed qTD must be rebuilt whole */
    for (u32 k = 16; k < 32; k += 4) usb_wr32(q + k, 0);
    usb_wr32(q + 8, ((u32)e->dt[n] << 31) | (1u << 8) | (3u << 10) | (1u << 15) | ((u32)d->rlen << 16) | 0x80u);
    usb_wr32(qh + 24, 0);
    usb_wr32(qh + 20, 1);
    USB_BARRIER();
    usb_wr32(qh + 16, q);
    return (int)got;
}

static void ehci_release(usb_hc_t *hc, usb_dev_t *d) {
    ehci_t *e = &ehci_st[hc->idx];
    u32 qh = hc->dma + EHCI_INT_BASE + 0x100u * (u32)d->slot;
    usb_wr32(qh + 16, 1); usb_wr32(qh + 24, 0x40u);
    e->used[d->slot] = 0;
}
#endif
