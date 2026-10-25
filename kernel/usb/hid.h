/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef USB_HID_H
#define USB_HID_H
#include "usb.h"

/* usb/hid.h -- the USB Human Interface Device class: turns the reports a keyboard, mouse or tablet sends into the same
 * key codes and pointer state the PS/2 drivers produce (keyboard_inject(), mouse_usb_report()/mouse_usb_abs()).
 *
 * Keyboards run in BOOT PROTOCOL: an 8-byte report (modifier bits, a reserved byte, up to six key usages) that every
 * keyboard supports, so no report descriptor is needed. A usage is turned into a PS/2 set-1 scancode and looked up in the
 * very same tables keyboard.h uses, which makes a USB 'A' and a PS/2 'A' the same byte by construction (Shift, Ctrl and
 * Right Alt included). USB sends no key repeat, so the driver repeats the last key itself: 500 ms delay, then every 40 ms.
 *
 * Mice run in boot protocol when they offer it (3 bytes: buttons, dx, dy). Everything else that points -- notably the
 * absolute "USB tablet" VirtualBox and QEMU present by default, which has no boot mode -- goes through a small HID report
 * descriptor parser that finds the button bits and the X/Y fields; relative fields move the pointer, absolute ones are
 * scaled to the screen. */

/* HID usage 0x04..0x38 -> PS/2 set-1 scancode (0 = no such key on a US layout) */
static const u8 hid_to_sc[53] = {
    0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,   /* a-m */
    0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C,   /* n-z */
    0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,                  /* 1-9, 0 */
    0x1C,0x01,0x0E,0x0F,0x39,0x0C,0x0D,0x1A,0x1B,0x2B,0x2B,             /* Enter Esc Bksp Tab Space - = [ ] \ (non-US #) */
    0x27,0x28,0x29,0x33,0x34,0x35                                       /* ; ' ` , . / */
};

static int hid_key_code(u8 usage) {
    if (usage >= 0x04 && usage <= 0x38) {
        u8 sc = hid_to_sc[usage - 4];
        if (sc) return (int)(unsigned char)(shift_held ? scancode_ascii_shift[sc] : scancode_ascii[sc]);
        return 0;
    }
    switch (usage) {
        case 0x4F: return KEY_RIGHT;
        case 0x50: return KEY_LEFT;
        case 0x51: return KEY_DOWN;
        case 0x52: return KEY_UP;
    }
    return 0;
}

static int hid_in_report(const u8 *r, u8 usage) {
    for (int i = 2; i < 8; i++) if (r[i] == usage) return 1;
    return 0;
}

static void hid_kbd_report(usb_dev_t *d, const u8 *r, int n) {
    if (n < 3) return;
    if (n > 8) n = 8;
    u8 cur[8] = {0,0,0,0,0,0,0,0};
    for (int i = 0; i < n; i++) cur[i] = r[i];
    u8 mods = cur[0];
    shift_held = (mods & 0x22) != 0;
    ctrl_held  = (mods & 0x11) != 0;
    if ((mods & 0x40) && !(d->last[0] & 0x40)) keyboard_inject(KEY_RALT);
    for (int i = 2; i < 8; i++) {
        u8 k = cur[i];
        if (k < 4) continue;                                  /* 0 = empty, 1..3 = rollover/error markers */
        if (hid_in_report(d->last, k)) continue;              /* held from before: not a new press */
        int code = hid_key_code(k);
        if (code) { keyboard_inject(code); d->rep_key = k; d->rep_next = usb_now_ms_v + 500; }
    }
    if (d->rep_key && !hid_in_report(cur, d->rep_key)) d->rep_key = 0;
    for (int i = 0; i < 8; i++) d->last[i] = cur[i];
}

static void hid_kbd_tick(usb_dev_t *d, u32 now) {
    if (!d->rep_key || (i32)(now - d->rep_next) < 0) return;
    int code = hid_key_code(d->rep_key);
    if (code) keyboard_inject(code);
    d->rep_next = now + 40;
}

/* bits [off, off+size) of a report, little-endian bit order, optionally sign-extended */
static i32 hid_bits(const u8 *r, int n, u32 off, u32 size, int sign) {
    u32 v = 0;
    for (u32 i = 0; i < size && i < 32; i++) {
        u32 bit = off + i;
        if ((int)(bit >> 3) >= n) break;
        if ((r[bit >> 3] >> (bit & 7)) & 1u) v |= 1u << i;
    }
    if (sign && size < 32 && (v & (1u << (size - 1)))) v |= ~0u << size;
    return (i32)v;
}

static void hid_mouse_report(usb_dev_t *d, const u8 *r, int n) {
    if (d->rid) {                                             /* the first byte is the report id: other reports are not ours */
        if (n < 1 || r[0] != d->rid) return;
        r++; n--;
    }
    int btn = (int)hid_bits(r, n, d->btn_off, d->btn_cnt > 8 ? 8 : d->btn_cnt, 0);
    int left = btn & 1, right = (btn >> 1) & 1;
    if (d->hid == HID_MOUSE) {
        int dx = (int)hid_bits(r, n, d->fx.off, d->fx.size, 1);
        int dy = (int)hid_bits(r, n, d->fy.off, d->fy.size, 1);
        mouse_usb_report(dx, dy, left, right);
        if (d->fwheel.size) { int wz = (int)hid_bits(r, n, d->fwheel.off, d->fwheel.size, 1); if (wz) mouse_wheel_add(wz > 0 ? -1 : 1); }   /* HID: positive = away from the user = up */
    } else {                                                  /* absolute: scale the field's range onto the screen */
        i32 x = hid_bits(r, n, d->fx.off, d->fx.size, d->fx.lmin < 0);
        i32 y = hid_bits(r, n, d->fy.off, d->fy.size, d->fy.lmin < 0);
        i32 rx = d->fx.lmax - d->fx.lmin, ry = d->fy.lmax - d->fy.lmin;
#ifdef MW_USB_DEBUG
        { static int dbg = 0; if (dbg++ < 40) { serial_puts("[USB] abs raw x="); serial_put_dec((u32)x); serial_puts(" y="); serial_put_dec((u32)y);
          serial_puts(" range "); serial_put_dec((u32)rx); serial_puts(" off "); serial_put_dec(d->fx.off); serial_puts("/"); serial_put_dec(d->fy.off);
          serial_puts(" size "); serial_put_dec(d->fx.size); serial_puts(" n="); serial_put_dec((u32)n); serial_puts(" btn@"); serial_put_dec(d->btn_off); serial_puts(" cnt "); serial_put_dec(d->btn_cnt); serial_puts(" left="); serial_put_dec((u32)left); serial_puts(" raw="); for (int q = 0; q < 6 && q < n; q++) serial_put_hex8(r[q]); serial_putc('\n'); } }
#endif
        x -= d->fx.lmin; y -= d->fy.lmin;
        while (rx > 0xFFFF || x > 0xFFFF) { rx >>= 1; x >>= 1; }
        while (ry > 0xFFFF || y > 0xFFFF) { ry >>= 1; y >>= 1; }
        if (rx <= 0) rx = 1;
        if (ry <= 0) ry = 1;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        int sx = (int)(((u32)x * (u32)(VGA_WIDTH - 1)) / (u32)rx);
        int sy = (int)(((u32)y * (u32)(VGA_HEIGHT - 1)) / (u32)ry);
        if (sx > VGA_WIDTH - 1) sx = VGA_WIDTH - 1;
        if (sy > VGA_HEIGHT - 1) sy = VGA_HEIGHT - 1;
        mouse_usb_abs(sx, sy, left, right);
        if (d->fwheel.size) { int wz = (int)hid_bits(r, n, d->fwheel.off, d->fwheel.size, 1); if (wz) mouse_wheel_add(wz > 0 ? -1 : 1); }
    }
}

static void hid_report(usb_dev_t *d, const u8 *r, int n) {
    if (d->hid == HID_KBD) hid_kbd_report(d, r, n);
    else if (d->hid == HID_MOUSE || d->hid == HID_TABLET) hid_mouse_report(d, r, n);
}

/* The fixed layouts of the boot protocol */
static void hid_boot_mouse_layout(usb_dev_t *d) {
    d->hid = HID_MOUSE; d->boot = 1; d->rid = 0;
    d->btn_off = 0; d->btn_cnt = 3;
    d->fx.off = 8;  d->fx.size = 8; d->fx.rel = 1; d->fx.lmin = -127; d->fx.lmax = 127;
    d->fy.off = 16; d->fy.size = 8; d->fy.rel = 1; d->fy.lmin = -127; d->fy.lmax = 127;
}

/* A compact HID report descriptor walk: finds the first run of buttons and the X and Y fields of ONE report (the report id
 * of the first thing that matches), and says whether the pointer is relative or absolute. Returns HID_MOUSE, HID_TABLET or
 * HID_NONE. It understands exactly the items that matter -- usage page, usages and usage ranges, logical min/max, report
 * size/count/id, and Input -- and skips everything else, which is what makes it small. */
static int hid_parse_report_desc(usb_dev_t *d, const u8 *p, int len) {
    u32 page = 0, rsize = 0, rcount = 0, rid = 0, bitpos = 0, trid = 0xFFFFFFFFu, nus = 0, umin = 0;
    u32 usages[16];
    i32 lmin = 0, lmax = 0;
    int have_range = 0, fx = 0, fy = 0, fb = 0, rel = 0;
    u32 rid_bits = 0;
    d->btn_cnt = 0;
    d->fwheel.size = 0;
    for (int i = 0; i < len; ) {
        u8 b = p[i++];
        if (b == 0xFE) { if (i < len) i += 1 + p[i]; continue; }       /* long item: skip */
        int sz = b & 3; if (sz == 3) sz = 4;
        int type = (b >> 2) & 3, tag = b >> 4;
        if (i + sz > len) break;
        u32 val = 0;
        for (int k = 0; k < sz; k++) val |= (u32)p[i + k] << (8 * k);
        i32 sval = (i32)val;
        if (sz == 1) sval = (i32)(signed char)val; else if (sz == 2) sval = (i32)(short)val;
        i += sz;
        if (type == 1) {                                               /* global */
            if (tag == 0) page = val;
            else if (tag == 1) lmin = sval;
            else if (tag == 2) lmax = (lmin >= 0) ? (i32)val : sval;
            else if (tag == 7) rsize = val;
            else if (tag == 8) { rid = val; bitpos = 0; }
            else if (tag == 9) rcount = val;
        } else if (type == 2) {                                        /* local */
            if (tag == 0 && nus < 16) usages[nus++] = val & 0xFFFFu;
            else if (tag == 1) { umin = val & 0xFFFFu; have_range = 1; }
        } else if (type == 0) {                                        /* main */
            if (tag == 8) {                                            /* Input */
                for (u32 k = 0; k < rcount; k++) {
                    u32 usage = nus ? (k < nus ? usages[k] : usages[nus - 1]) : (have_range ? umin + k : 0);
                    if (!(val & 1u) && (trid == 0xFFFFFFFFu || trid == rid)) {
                        if (page == 9) {
                            if (!fb) { fb = 1; d->btn_off = (u16)bitpos; d->btn_cnt = 0; if (trid == 0xFFFFFFFFu) trid = rid; }
                            if (d->btn_cnt < 8) d->btn_cnt++;
                        } else if (page == 1 && usage == 0x38 && !d->fwheel.size) {
                            d->fwheel.off = (u16)bitpos; d->fwheel.size = (u8)rsize; d->fwheel.rel = 1; d->fwheel.lmin = lmin; d->fwheel.lmax = lmax;
                        } else if (page == 1 && (usage == 0x30 || usage == 0x31)) {
                            hid_field_t *f = usage == 0x30 ? &d->fx : &d->fy;
                            if (!(usage == 0x30 ? fx : fy)) {
                                f->off = (u16)bitpos; f->size = (u8)rsize; f->rel = (u8)((val >> 2) & 1u); f->lmin = lmin; f->lmax = lmax;
                                if (usage == 0x30) { fx = 1; rel = f->rel; } else fy = 1;
                                if (trid == 0xFFFFFFFFu) trid = rid;
                            }
                        }
                    }
                    bitpos += rsize;
                }
                if (trid == rid && bitpos > rid_bits) rid_bits = bitpos;
            }
            nus = 0; have_range = 0; umin = 0;
        }
    }
    if (!fx || !fy || trid == 0xFFFFFFFFu) return HID_NONE;
    d->rid = (u8)trid;
    d->rlen = (u16)(((rid_bits + 7) >> 3) + (trid ? 1 : 0));
    d->boot = 0;
    return rel ? HID_MOUSE : HID_TABLET;
}
#endif
