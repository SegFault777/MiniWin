/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MOUSE_H
#define MOUSE_H
#include "io.h"

/* mouse.h -- the mouse: initialises the auxiliary device of the i8042 controller and decodes its 3-byte packets into movement
 * and button state, by polling. A USB mouse or tablet (usb/hid.h) reports through mouse_usb_report()/mouse_usb_abs() below
 * and shows up in the very same mouse_dx/dy/left/click_event variables, so the desktop code has one mouse to think about. */

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

static inline void ps2_wait_input_clear(void) {
    /* Spin until the controller says "yeah okay, go ahead and write."
     * The timeout exists so we don't sit here forever like an idiot if
     * the hardware never answers. */
    int timeout = 100000;
    while ((inb(PS2_STATUS) & 0x02) && timeout--) { }
}

static inline void ps2_wait_output_full(void) {
    /* Same deal but for reading: wait for the controller to actually
     * have something for us before we go grabbing garbage off the bus. */
    int timeout = 100000;
    while (!(inb(PS2_STATUS) & 0x01) && timeout--) { }
}

static inline void ps2_write_cmd(u8 cmd) {
    ps2_wait_input_clear();
    outb(PS2_CMD, cmd);
}

static inline void ps2_write_data(u8 data) {
    ps2_wait_input_clear();
    outb(PS2_DATA, data);
}

static inline u8 ps2_read_data(void) {
    ps2_wait_output_full();
    return inb(PS2_DATA);
}

/* Yell a byte at the mouse (PS/2 port 2) and wait for it to grunt back an
 * ACK. If it doesn't, that's a problem for future us. */
static inline void mouse_write(u8 val) {
    ps2_write_cmd(0xD4);       /* "oi, next byte's for the mouse" */
    ps2_write_data(val);
    (void)ps2_read_data();    /* the grunt */
}

static int mouse_dx = 0, mouse_dy = 0;   /* how far the mouse moved since we last asked */

/* POINTER SPEED (Settings > Mouse): 0 = slow (x1/2), 1 = normal, 2 = fast (x2), 3 = faster (x3). Relative motion is scaled by
 * num/den with the remainder carried to the next report, so "slow" still moves on a 1-count nudge every second time instead of
 * never. (Tablets report absolute positions and are not scaled.) */
static int mouse_speed = 1;
static int mouse_rem_x = 0, mouse_rem_y = 0;
static inline int mouse_scale(int d, int *rem) {
    static const signed char num[4] = { 1, 1, 2, 3 }, den[4] = { 2, 1, 1, 1 };
    int n = num[mouse_speed & 3], m = den[mouse_speed & 3];
    int a = *rem + d * n, out = a / m;
    *rem = a - out * m;
    return out;
}
static int mouse_left = 0, mouse_right = 0;
static int mouse_left_prev = 0;           /* left-button state last time we looked */
static int mouse_click_event = 0;         /* 1 if the left button went down THIS poll -- a real click, not just "still held" */
static int mouse_release_event = 0;       /* 1 if the left button went UP this poll -- the other half of a press-and-release */
static u8  mouse_packet[3];
static int mouse_packet_idx = 0;

/* What the USB HID driver leaves here between two mouse_poll() calls. Relative mice add into the dx/dy accumulators; a
 * tablet (an absolute pointer, the kind VirtualBox and VMware present by default) sets a target position instead. */
static int mouse_usb_dx = 0, mouse_usb_dy = 0, mouse_usb_left = 0, mouse_usb_right = 0, mouse_usb_left_prev = 0;
static int mouse_usb_pending = 0;
static int mouse_wheel = 0;                    /* notches since the desktop last asked; > 0 = scroll DOWN. Both the PS/2 IntelliMouse and USB mice add here */
static int mouse_packet_len = 3;               /* 4 once the PS/2 mouse has agreed to be an IntelliMouse (it then has a wheel byte) */
static inline void mouse_wheel_add(int n) { mouse_wheel += n; }
static int mouse_abs_x = 0, mouse_abs_y = 0, mouse_abs_valid = 0;
static inline void mouse_usb_report(int dx, int dy, int left, int right) {
    mouse_usb_dx += dx; mouse_usb_dy += dy;
    mouse_usb_left = left; mouse_usb_right = right;
    mouse_usb_pending = 1;
}
static inline void mouse_usb_abs(int x, int y, int left, int right) {
    mouse_abs_x = x; mouse_abs_y = y; mouse_abs_valid = 1;
    mouse_usb_left = left; mouse_usb_right = right;
    mouse_usb_pending = 1;
}

static inline void mouse_init(void) {
    /* Tell the PS/2 controller "yes, there is in fact a rodent attached" */
    ps2_write_cmd(0xA8);

    /* Flip the bits that let the mouse's clock/IRQ actually run. Skip
     * this and the mouse just sits there like a dead fish -- ask me how
     * we know. */
    ps2_write_cmd(0x20);              /* gimme the config byte */
    u8 status = ps2_read_data();
    status |= 0x02;                    /* enable IRQ12 (bit1) */
    status &= ~0x20;                   /* un-mute the mouse clock (bit5=0) */
    ps2_write_cmd(0x60);              /* take it back, but better */
    ps2_write_data(status);

    mouse_write(0xF6);   /* reset to sane defaults */

    /* The IntelliMouse handshake: set the sample rate to 200, 100, 80 in a row -- a plain mouse shrugs, a wheel mouse
     * takes it as a secret knock and answers "what are you?" with 3 instead of 0, and from then on sends a 4th byte per
     * packet whose low nibble is the wheel. */
    mouse_write(0xF3); mouse_write(200);
    mouse_write(0xF3); mouse_write(100);
    mouse_write(0xF3); mouse_write(80);
    ps2_write_cmd(0xD4); ps2_write_data(0xF2);            /* "what are you?" */
    (void)ps2_read_data();                                /* the ACK */
    u8 id = ps2_read_data();
    mouse_packet_len = (id == 3) ? 4 : 3;

    mouse_write(0xF4);   /* "start yapping" -- enable streaming reports */

    mouse_packet_idx = 0;
}

/* Call this every loop iteration, no exceptions. Hoovers up every mouse
 * byte currently sitting in the PS/2 buffer, glues them into 3-byte
 * packets, and updates dx/dy/buttons once a full packet's in hand. We
 * drain the WHOLE queue in one go instead of one byte at a time --
 * learned that one the hard way, see the framing comment below for the
 * gory details of how badly that went the first time.
 * Returns 1 if we actually got at least one full packet, 0 if the mouse
 * had nothing to say for itself. */
static inline int mouse_poll(void) {
    int got_any = 0;
    int acc_dx = 0, acc_dy = 0;
    int click_event = 0;
    int release_event = 0;

    while (1) {
        u8 status = inb(PS2_STATUS);
        if (status == 0xFF) break;           /* an empty bus reads all ones: no i8042 here (USB-only machines), not a byte waiting */
        if (!(status & 0x01)) break;         /* nothing waiting, move along */
        if (!(status & 0x20)) {
            /* This byte belongs to the keyboard, not the mouse -- bail
             * out and let keyboard_poll_key() have it. Steal it here and
             * you get random garbage typed into the document every time
             * someone wiggles the mouse. We know. We did that. It was
             * not fun to debug. */
            break;
        }

        u8 b = inb(PS2_DATA);

        /* Sync check: a real packet's first byte always has bit3 set. If
         * we're not lined up right (e.g. we started listening mid-packet
         * at boot), chuck bytes until we find a sane starting point
         * instead of confidently interpreting nonsense as a 400px jump. */
        if (mouse_packet_idx == 0 && !(b & 0x08)) {
            continue; /* nope, keep looking */
        }

        mouse_packet[mouse_packet_idx++] = b;
        if (mouse_packet_idx < mouse_packet_len) continue;
        mouse_packet_idx = 0;

        u8 flags = mouse_packet[0];
        if (mouse_packet_len == 4) {                  /* the wheel byte: a signed nibble, positive = wheel towards the user = scroll down */
            int z = mouse_packet[3] & 0x0F;
            if (z & 8) z -= 16;
            mouse_wheel += z;
        }

        /* Button state gets processed no matter what happens to the
         * movement bytes below. A click can ride along on the exact same
         * packet as a garbage/overflowed movement, and dropping the click
         * just because the movement was junk would be a genuinely dumb
         * bug to ship. */
        int this_left = flags & 0x01;
        if (this_left && !mouse_left_prev) click_event = 1;
        if (!this_left && mouse_left_prev) release_event = 1;
        mouse_left_prev = this_left;

        mouse_left  = this_left;
        mouse_right = flags & 0x02;
        got_any = 1;

        /* If the overflow bits are set, the dx/dy bytes are basically
         * lying to us -- toss the movement (but keep the button state
         * from above, that part's still legit). This is the actual fix
         * for the infamous "flick the mouse once, cursor teleports to
         * the corner" bug. Clamping alone wasn't enough; you have to
         * just refuse to believe the bad data. */
        if (flags & 0xC0) {
            continue;
        }

        int dx = mouse_packet[1];
        int dy = mouse_packet[2];
        if (flags & 0x10) dx -= 256;   /* sign bit for X */
        if (flags & 0x20) dy -= 256;   /* sign bit for Y */

        /* Belt-and-suspenders clamp: even a "valid" packet shouldn't
         * fling the cursor across the whole desktop from one flick. If
         * framing ever drifts (dropped byte, weird USB/PS2 shenanigans),
         * a garbage byte can look like a huge signed value and send the
         * cursor to Narnia. This caps the blast radius of any one bad
         * packet without us having to prove exactly which byte lied. */
        if (dx > 127) dx = 127;
        if (dx < -127) dx = -127;
        if (dy > 127) dy = 127;
        if (dy < -127) dy = -127;

        acc_dx += dx;
        acc_dy += -dy;  /* Flip the damn Y axis. This hardware reports "up"
                         * as a positive dy, but our screen thinks smaller
                         * y = up, so without this the cursor moves like
                         * it's possessed -- confirmed by actually testing
                         * on real WSLg/host mouse hardware and going
                         * "why is this thing upside down." */
    }

    if (mouse_usb_pending) {                      /* a USB mouse/tablet spoke since the last poll: merge it in */
        mouse_usb_pending = 0;
        acc_dx += mouse_usb_dx; acc_dy += mouse_usb_dy;
        mouse_usb_dx = 0; mouse_usb_dy = 0;
        if (mouse_usb_left && !mouse_usb_left_prev) click_event = 1;
        if (!mouse_usb_left && mouse_usb_left_prev) release_event = 1;
        mouse_usb_left_prev = mouse_usb_left;
        mouse_left = mouse_usb_left;
        mouse_right = mouse_usb_right;
        got_any = 1;
    }
    mouse_dx = mouse_scale(acc_dx, &mouse_rem_x);
    mouse_dy = mouse_scale(acc_dy, &mouse_rem_y);
    mouse_click_event = click_event;
    mouse_release_event = release_event;
    return got_any;
}

#endif
