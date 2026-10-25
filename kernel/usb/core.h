/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef USB_CORE_H
#define USB_CORE_H
#include "usb.h"
#include "xhci.h"
#include "ehci.h"
#include "ohci.h"
#include "hid.h"

/* usb/core.h -- what ties the three host controller drivers and the HID class driver together: finding the controllers on
 * the PCI bus, enumerating every device that shows up on a root port (address, descriptors, configuration), picking the HID
 * function out of its configuration, and the one poll the main loop calls to move reports along and notice hot-plugging.
 *
 * Not implemented, and said so on purpose: external hubs (a device behind a hub is invisible), mass storage, and anything
 * that is not HID. UHCI controllers (the Intel companion kind) are listed in the log but not driven. */

static const char *const usb_hc_names[] = { "none", "ohci", "ehci", "xhci" };

static int usb_ctrl(usb_dev_t *d, u8 bm, u8 req, u16 val, u16 idx, u16 len) {
    u8 s[8];
    usb_setup_pkt(s, bm, req, val, idx, len);
    usb_hc_t *hc = &usb_hcs[d->hc];
    int in = (bm & 0x80) != 0;
    switch (hc->type) {
        case USB_HC_XHCI: return xhci_control(hc, d, s, len, in);
        case USB_HC_EHCI: return ehci_control(hc, d, s, len, in);
        case USB_HC_OHCI: return ohci_control(hc, d, s, len, in);
    }
    return -1;
}

static void usb_release(usb_dev_t *d) {
    usb_hc_t *hc = &usb_hcs[d->hc];
    if (d->hid) {
        switch (hc->type) {
            case USB_HC_XHCI: xhci_release(hc, d); break;
            case USB_HC_EHCI: ehci_release(hc, d); break;
            case USB_HC_OHCI: ohci_release(hc, d); break;
        }
        if (d->hid == HID_KBD) { shift_held = 0; ctrl_held = 0; }
    } else if (hc->type == USB_HC_XHCI) xhci_release(hc, d);
    d->present = 0; d->hid = HID_NONE;
}

/* the interface candidates found while walking a configuration descriptor */
typedef struct { u8 num, sub, proto, ep, interval; u16 mps, rdesc_len; } usb_cand_t;

static void usb_enumerate(int hci, int port, int speed) {
    usb_hc_t *hc = &usb_hcs[hci];
    int di = -1;
    for (int i = 0; i < USB_MAX_DEVS; i++) if (!usb_devs[i].present) { di = i; break; }
    if (di < 0) { usb_log("device table full, device ignored"); return; }
    usb_dev_t *d = &usb_devs[di];
    usb_zero(d, sizeof *d);
    d->present = 1; d->hc = (u8)hci; d->port = (u8)port; d->speed = (u8)speed;

    int ok = 0;
    switch (hc->type) {
        case USB_HC_XHCI: ok = xhci_address(hc, d); break;
        case USB_HC_EHCI: ok = ehci_address(hc, d); break;
        case USB_HC_OHCI: ok = ohci_address(hc, d); break;
    }
    if (!ok) { usb_log("could not address a device"); goto fail; }
    if (hc->type != USB_HC_XHCI) {                                    /* ehci/ohci: we hand out the address ourselves */
        u8 a = (u8)(di + 1);
        if (usb_ctrl(d, 0x00, 5, a, 0, 0) < 0) { usb_log("SET_ADDRESS failed"); goto fail; }
        delay_ms(10);
        d->addr = a;
    }
    if (usb_ctrl(d, 0x80, 6, 0x0100, 0, 8) < 8) { usb_log("no device descriptor"); goto fail; }
    {
        u32 mps = usb_cbuf[7];
        if (speed == USB_SPEED_SUPER) mps = (mps >= 9 && mps <= 10) ? (1u << mps) : 512;
        if (mps == 8 || mps == 16 || mps == 32 || mps == 64 || mps == 512) {
            if (mps != d->mps0) { if (hc->type == USB_HC_XHCI) xhci_set_mps0(hc, d, mps); else d->mps0 = (u16)mps; }
        }
    }
    if (usb_ctrl(d, 0x80, 6, 0x0100, 0, 18) < 18) { usb_log("short device descriptor"); goto fail; }
    d->cls = usb_cbuf[4];
    d->vid = (u16)(usb_cbuf[8] | (usb_cbuf[9] << 8));
    d->pid = (u16)(usb_cbuf[10] | (usb_cbuf[11] << 8));

    if (usb_ctrl(d, 0x80, 6, 0x0200, 0, 9) < 9) { usb_log("no configuration descriptor"); goto fail; }
    u32 total = (u32)usb_cbuf[2] | ((u32)usb_cbuf[3] << 8);
    d->cfg = usb_cbuf[5];
    if (total > USB_CBUF_SIZE - 1) total = USB_CBUF_SIZE - 1;
    if (total < 9) total = 9;
    if (usb_ctrl(d, 0x80, 6, 0x0200, 0, (u16)total) < 9) { usb_log("could not read the configuration"); goto fail; }

    usb_cand_t cand[6];
    int nc = -1, in_hid = 0;
    for (u32 off = 9; off + 2 <= total; ) {
        u8 len = usb_cbuf[off], typ = usb_cbuf[off + 1];
        if (len < 2 || off + len > total) break;
        const u8 *p = usb_cbuf + off;
        if (typ == 4) {                                               /* interface */
            in_hid = 0;
            if (p[5] == 3 && p[3] == 0 && nc < 5) {                   /* HID, alternate setting 0 */
                nc++; in_hid = 1;
                cand[nc].num = p[2]; cand[nc].sub = p[6]; cand[nc].proto = p[7];
                cand[nc].ep = 0; cand[nc].mps = 0; cand[nc].interval = 0; cand[nc].rdesc_len = 0;
            } else {
                in_hid = 0;                                           /* endpoints that follow belong to something else */
            }
        } else if (typ == 0x21 && in_hid && len >= 9) {              /* HID descriptor: the report descriptor length */
            if (p[5] >= 1 && p[6] == 0x22) cand[nc].rdesc_len = (u16)(p[7] | (p[8] << 8));   /* bNumDescriptors, bDescriptorType, wDescriptorLength */
        } else if (typ == 5 && in_hid) {                             /* endpoint */
            if ((p[3] & 3) == 3 && (p[2] & 0x80) && cand[nc].ep == 0) {
                cand[nc].ep = p[2] & 15; cand[nc].mps = (u16)((p[4] | (p[5] << 8)) & 0x7FF); cand[nc].interval = p[6];
            }
        }
        off += len;
    }
    nc++;

    int pick = -1, kind = HID_NONE;
    for (int i = 0; i < nc && pick < 0; i++) if (cand[i].ep && cand[i].sub == 1 && cand[i].proto == 1) { pick = i; kind = HID_KBD; }
    for (int i = 0; i < nc && pick < 0; i++) if (cand[i].ep && cand[i].sub == 1 && cand[i].proto == 2) { pick = i; kind = HID_MOUSE; }
    if (pick >= 0) {
        d->iface = cand[pick].num; d->ep = cand[pick].ep; d->ep_mps = cand[pick].mps; d->ep_interval = cand[pick].interval;
        if (kind == HID_MOUSE) {
            hid_boot_mouse_layout(d);
            /* Prefer the mouse's own report layout when it can be read: that is where the wheel is (the boot report has no
             * wheel byte). If the descriptor cannot be read or understood, the fixed boot layout above stays. */
            u16 rl = cand[pick].rdesc_len > USB_CBUF_SIZE ? USB_CBUF_SIZE : cand[pick].rdesc_len;
            if (rl >= 4 && usb_ctrl(d, 0x81, 6, 0x2200, cand[pick].num, rl) >= 4) {
                usb_dev_t t = *d;
                if (hid_parse_report_desc(&t, usb_cbuf, rl) == HID_MOUSE) { *d = t; d->hid = HID_MOUSE; d->boot = 0; }
            }
        } else { d->hid = HID_KBD; d->boot = 1; }
    } else {                                                          /* a mouse or tablet with no boot mode: read its report descriptor */
        for (int i = 0; i < nc && pick < 0; i++) {
            if (!cand[i].ep || !cand[i].rdesc_len) continue;
            u16 rl = cand[i].rdesc_len > USB_CBUF_SIZE ? USB_CBUF_SIZE : cand[i].rdesc_len;
            if (usb_ctrl(d, 0x81, 6, 0x2200, cand[i].num, rl) < 4) continue;
            u8 save_ep = d->ep; u16 save_mps = d->ep_mps;
            int k = hid_parse_report_desc(d, usb_cbuf, rl);
            if (k == HID_NONE) { d->ep = save_ep; d->ep_mps = save_mps; continue; }
            d->hid = (u8)k; d->iface = cand[i].num; d->ep = cand[i].ep; d->ep_mps = cand[i].mps; d->ep_interval = cand[i].interval;
            pick = i;
        }
    }
    if (pick < 0 || d->hid == HID_NONE) {
        serial_puts("[USB] device "); serial_put_hex16(d->vid); serial_putc(':'); serial_put_hex16(d->pid);
        serial_puts(" has no keyboard/mouse function this kernel can use (class "); serial_put_hex8(d->cls); serial_puts(")\n");
        goto fail;
    }
    if (d->ep_mps == 0 || d->ep_mps > 64) d->ep_mps = d->ep_mps ? 64 : 8;
    d->rlen = d->boot ? d->ep_mps : (d->rlen && d->rlen <= d->ep_mps ? d->rlen : d->ep_mps);
    if (d->rlen > 64) d->rlen = 64;

    if (usb_ctrl(d, 0x00, 9, d->cfg, 0, 0) < 0) { usb_log("SET_CONFIGURATION failed"); goto fail; }
    if (d->boot) usb_ctrl(d, 0x21, 0x0B, 0, d->iface, 0);             /* SET_PROTOCOL: boot (some devices stall: fine) */
    usb_ctrl(d, 0x21, 0x0A, 0, d->iface, 0);                          /* SET_IDLE: only report changes */
    switch (hc->type) {
        case USB_HC_XHCI: ok = xhci_setup_intr(hc, d); break;
        case USB_HC_EHCI: ok = ehci_setup_intr(hc, d); break;
        case USB_HC_OHCI: ok = ohci_setup_intr(hc, d); break;
    }
    if (!ok) { usb_log("could not start the interrupt endpoint"); goto fail; }
    serial_puts("[USB] ");
    serial_puts(d->hid == HID_KBD ? "keyboard" : d->hid == HID_MOUSE ? "mouse" : "tablet");
    serial_puts(" ready on "); serial_puts(usb_hc_names[hc->type]); serial_put_dec(hc->idx);
    serial_puts(" port "); serial_put_dec((u32)port + 1);
    serial_puts(" id "); serial_put_hex16(d->vid); serial_putc(':'); serial_put_hex16(d->pid);
    serial_puts(d->boot ? " (boot protocol)\n" : " (report protocol)\n");
    return;
fail:
    if (hc->type == USB_HC_XHCI && d->slot) xhci_release(hc, d);
    d->present = 0; d->hid = HID_NONE;
}

static void usb_port_changed(int hci, int port, int first) {
    usb_hc_t *hc = &usb_hcs[hci];
    int speed = 0, r = 0;
    switch (hc->type) {
        case USB_HC_XHCI: r = xhci_port_scan(hc, port, first, &speed); break;
        case USB_HC_EHCI: r = ehci_port_scan(hc, port, first, &speed); break;
        case USB_HC_OHCI: r = ohci_port_scan(hc, port, first, &speed); break;
    }
    if (r == 2) {
        for (int i = 0; i < USB_MAX_DEVS; i++)
            if (usb_devs[i].present && usb_devs[i].hc == hci && usb_devs[i].port == port) {
                usb_log("device removed");
                usb_release(&usb_devs[i]);
            }
    } else if (r == 1) {
        for (int i = 0; i < USB_MAX_DEVS; i++)                        /* a re-connect on a port still on the books: drop the old entry */
            if (usb_devs[i].present && usb_devs[i].hc == hci && usb_devs[i].port == port) usb_release(&usb_devs[i]);
        usb_enumerate(hci, port, speed);
    }
}

static void usb_scan_all(int first) {
    for (int h = 0; h < usb_hc_count; h++)
        for (int p = 0; p < usb_hcs[h].nports; p++) usb_port_changed(h, p, first);
}

static void usb_init(void) {
    usb_hc_count = 0;
    static const u8 order[3] = { 0x30, 0x10, 0x20 };                  /* xHCI first, then OHCI, then EHCI (which hands ports to OHCI) */
    for (int o = 0; o < 3; o++) {
        for (int i = 0; i < pci_device_count && usb_hc_count < (int)MW_USB_HC_MAX; i++) {
            pci_device_t *pd = &pci_devices[i];
            if (pd->class_code != 0x0C || pd->subclass != 0x03 || pd->prog_if != order[o]) continue;
            usb_hc_t *hc = &usb_hcs[usb_hc_count];
            usb_zero(hc, sizeof *hc);
            hc->idx = (u8)usb_hc_count; hc->bus = pd->bus; hc->slot = pd->slot; hc->func = pd->func;
            hc->dma = MW_USB_DMA_ADDR + MW_USB_HC_STRIDE * (u32)usb_hc_count;
            int ok = 0;
            if (order[o] == 0x30) { hc->type = USB_HC_XHCI; ok = xhci_init(hc, pd); }
            else if (order[o] == 0x10) { hc->type = USB_HC_OHCI; ok = ohci_init(hc, pd); }
            else { hc->type = USB_HC_EHCI; ok = ehci_init(hc, pd); }
            if (!ok) { usb_log("controller failed to start, ignored"); hc->type = USB_HC_NONE; continue; }
            serial_puts("[USB] "); serial_puts(usb_hc_names[hc->type]); serial_put_dec(hc->idx);
            serial_puts(" up at "); serial_put_hex32(hc->mmio); serial_puts(", "); serial_put_dec(hc->nports); serial_puts(" port(s)\n");
            usb_hc_count++;
        }
    }
    for (int i = 0; i < pci_device_count; i++)
        if (pci_devices[i].class_code == 0x0C && pci_devices[i].subclass == 0x03 && pci_devices[i].prog_if == 0x00)
            usb_log("a UHCI controller is present but UHCI is not supported");
    if (!usb_hc_count) return;
    usb_now_last = pit_read();
    usb_scan_all(1);                                                  /* devices already plugged in */
    delay_ms(100);
    usb_scan_all(0);                                                  /* what the EHCI companion hand-off just produced */
}

static u32 usb_scan_last = 0;
static void usb_poll(void) {
    if (!usb_hc_count) return;                                        /* no controllers: this whole thing costs one compare */
    u32 now = usb_now_ms();
    for (int h = 0; h < usb_hc_count; h++) {
        usb_hc_t *hc = &usb_hcs[h];
        if (hc->type == USB_HC_XHCI) xhci_events(hc);
        else if (hc->type == USB_HC_OHCI) ohci_events(hc);
    }
    for (int i = 0; i < USB_MAX_DEVS; i++) {
        usb_dev_t *d = &usb_devs[i];
        if (!d->present || !d->hid) continue;
        usb_hc_t *hc = &usb_hcs[d->hc];
        u8 buf[64];
        int n = -1;
        switch (hc->type) {
            case USB_HC_XHCI: n = xhci_intr_poll(hc, d, buf); break;
            case USB_HC_EHCI: n = ehci_intr_poll(hc, d, buf); break;
            case USB_HC_OHCI: n = ohci_intr_poll(hc, d, buf); break;
        }
        if (d->dead) { usb_log("device stopped answering, dropped"); usb_release(d); continue; }
        if (n > 0) hid_report(d, buf, n);
        if (d->hid == HID_KBD) hid_kbd_tick(d, now);
    }
#ifdef MW_USB_DEBUG
    { static u32 dl = 0; if ((i32)(now - dl) >= 3000) { dl = now;
      for (int i = 0; i < USB_MAX_DEVS; i++) { usb_dev_t *d = &usb_devs[i]; if (!d->present || !d->hid) continue;
        usb_hc_t *hc = &usb_hcs[d->hc];
        if (hc->type == USB_HC_EHCI) {
            u32 base = hc->dma + EHCI_INT_BASE + 0x100u * (u32)d->slot;
            serial_puts("[USB] dbg ehci qh:"); for (int k = 0; k < 8; k++) { serial_putc(' '); serial_put_hex32(usb_rd32(base + 4u * (u32)k)); }
            serial_puts(" qtd:"); for (int k = 0; k < 4; k++) { serial_putc(' '); serial_put_hex32(usb_rd32(base + 0x40 + 4u * (u32)k)); }
            serial_puts(" sts="); serial_put_hex32(usb_rd32(ehci_st[hc->idx].op + 4)); serial_puts(" cmd="); serial_put_hex32(usb_rd32(ehci_st[hc->idx].op));
            serial_puts(" frindex="); serial_put_hex32(usb_rd32(ehci_st[hc->idx].op + 0xC)); serial_putc('\n');
        } } } }
#endif
    if ((i32)(now - usb_scan_last) >= 250) { usb_scan_last = now; usb_scan_all(0); }   /* hot-plug */
}
#endif
