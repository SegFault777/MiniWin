#ifndef KEYBOARD_H
#define KEYBOARD_H
#include "io.h"

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64

/* A few special keys (arrows, Right Alt) don't fit in one byte -- the
 * keyboard sends a 0xE0 "heads up, extended key incoming" byte first,
 * then the actual code. Two-byte gang. */
#define SC_EXT_PREFIX  0xE0
#define SC_UP          0x48
#define SC_DOWN        0x50
#define SC_LEFT        0x4B
#define SC_RIGHT       0x4D
#define SC_RALT        0x38

/* What keyboard_poll_key() hands back for keys that aren't plain ASCII */
#define KEY_NONE   0
#define KEY_UP     -1
#define KEY_DOWN   -2
#define KEY_LEFT   -3
#define KEY_RIGHT  -4
#define KEY_RALT   -6

/* US QWERTY scancode set 1 -> ASCII (unshifted). Index straight into this
 * with the raw scancode, no lookup table nonsense. */
static const char scancode_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, /* ctrl */
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, /* left shift */
    '\\','z','x','c','v','b','n','m',',','.','/',
    0, /* right shift */
    '*',
    0, /* alt */
    ' ',
    0, /* caps lock */
    0,0,0,0,0,0,0,0,0,0, /* F1-F10, none of your business here */
    0, /* num lock */
    0, /* scroll lock */
    0,0,0,0,0,0,0,0,0,0,0,0,0, /* numpad stuff we're ignoring */
};

/* Same table but shift's held down -- SHOUTING, symbols, the works. */
static const char scancode_ascii_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,
    'A','S','D','F','G','H','J','K','L',':','"','~',
    0,
    '|','Z','X','C','V','B','N','M','<','>','?',
    0,
    '*',
    0,
    ' ',
    0,
    0,0,0,0,0,0,0,0,0,0,
    0,
    0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,
};

static int shift_held = 0;
static int ctrl_held = 0;
static int ext_prefix_pending = 0;

static inline int keyboard_ctrl_held(void) { return ctrl_held; }

/* Returns:
 *   > 0   : an actual ASCII character, somebody pressed a real key
 *   KEY_UP/DOWN/LEFT/RIGHT/F7/RALT (negative) : a special key, no ASCII for you
 *   0     : nothing happened, or it was a release, or just a modifier --
 *           either way, move along
 * Non-blocking, call it every loop iteration and don't be shy about it. */
static inline int keyboard_poll_key(void) {
    u8 status = inb(KBD_STATUS_PORT);
    if (!(status & 0x01)) return KEY_NONE; /* keyboard's got nothing to say */
    if (status & 0x20) {
        /* This byte is the MOUSE's, not the keyboard's -- bit 5 is how
         * you tell them apart since they share the same output buffer.
         * Reading it here instead of leaving it for mouse_poll() is
         * exactly how we ended up with random '7', '0', ''' characters
         * typing themselves whenever someone so much as breathed on the
         * mouse. Not doing that again. */
        return KEY_NONE;
    }

    u8 sc = inb(KBD_DATA_PORT);

    if (sc == SC_EXT_PREFIX) { ext_prefix_pending = 1; return KEY_NONE; }

    if (ext_prefix_pending) {
        ext_prefix_pending = 0;
        if (sc & 0x80) return KEY_NONE; /* just the key letting go, don't care */
        switch (sc) {
            case SC_UP:    return KEY_UP;
            case SC_DOWN:  return KEY_DOWN;
            case SC_LEFT:  return KEY_LEFT;
            case SC_RIGHT: return KEY_RIGHT;
            case SC_RALT:  return KEY_RALT;
            default:       return KEY_NONE;
        }
    }

    if (sc == 0x2A || sc == 0x36) { shift_held = 1; return KEY_NONE; }  /* shift down */
    if (sc == 0xAA || sc == 0xB6) { shift_held = 0; return KEY_NONE; }  /* shift up */
    if (sc == 0x1D) { ctrl_held = 1; return KEY_NONE; }                 /* (left) ctrl down */
    if (sc == 0x9D) { ctrl_held = 0; return KEY_NONE; }                 /* (left) ctrl up */

    if (sc & 0x80) return KEY_NONE; /* a release -- we only care about presses */
    if (sc >= 128) return KEY_NONE; /* scancode out of range, not our problem */

    char c = shift_held ? scancode_ascii_shift[sc] : scancode_ascii[sc];
    return (int)(unsigned char)c;
}

/* Old dumb wrapper from before arrow keys existed. Kept around because
 * something might still call it and I'm not in the mood to go hunting. */
static inline char keyboard_poll(void) {
    int k = keyboard_poll_key();
    if (k > 0) return (char)k;
    return 0;
}

#endif
