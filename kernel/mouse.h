#ifndef MOUSE_H
#define MOUSE_H
#include "io.h"

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
static int mouse_left = 0, mouse_right = 0;
static int mouse_left_prev = 0;           /* left-button state last time we looked */
static int mouse_click_event = 0;         /* 1 if the left button went down THIS poll -- a real click, not just "still held" */
static u8  mouse_packet[3];
static int mouse_packet_idx = 0;

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

    while (1) {
        u8 status = inb(PS2_STATUS);
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
        if (mouse_packet_idx < 3) continue;
        mouse_packet_idx = 0;

        u8 flags = mouse_packet[0];

        /* Button state gets processed no matter what happens to the
         * movement bytes below. A click can ride along on the exact same
         * packet as a garbage/overflowed movement, and dropping the click
         * just because the movement was junk would be a genuinely dumb
         * bug to ship. */
        int this_left = flags & 0x01;
        if (this_left && !mouse_left_prev) click_event = 1;
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

    mouse_dx = acc_dx;
    mouse_dy = acc_dy;
    mouse_click_event = click_event;
    return got_any;
}

#endif
