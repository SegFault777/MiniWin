#ifndef VGA_H
#define VGA_H
#include "io.h"

#define VGA_WIDTH  320
#define VGA_HEIGHT 200
#define VGA_MEMORY ((u8*)0xA0000)

/* Slam the VGA controller into Mode 13h (320x200, 256 color, one nice
 * flat linear framebuffer) by hand-writing the whole register sequence
 * ourselves, because BIOS int 0x10 stopped answering our calls the
 * second we entered protected mode. Rude, but that's the deal. */
static inline void vga_set_mode13h(void) {
    /* Misc output register */
    outb(0x3C2, 0x63);

    /* Sequencer registers */
    static const u8 seq[5] = {0x03, 0x01, 0x0F, 0x00, 0x0E};
    outb(0x3C4, 0x00); outb(0x3C5, seq[0]);
    outb(0x3C4, 0x01); outb(0x3C5, seq[1]);
    outb(0x3C4, 0x02); outb(0x3C5, seq[2]);
    outb(0x3C4, 0x03); outb(0x3C5, seq[3]);
    outb(0x3C4, 0x04); outb(0x3C5, seq[4]);

    /* Unlock CRTC registers -- they're write-protected by default, which
     * is a fantastic way to waste twenty minutes wondering why half your
     * settings aren't taking effect */
    outb(0x3D4, 0x11); outb(0x3D5, inb(0x3D5) & 0x7F);

    static const u8 crtc[25] = {
        0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,
        0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,
        0x9C,0x0E,0x8F,0x28,0x40,0x96,0xB9,0xA3,
        0xFF
    };
    for (u8 i = 0; i < 25; i++) {
        outb(0x3D4, i);
        outb(0x3D5, crtc[i]);
    }

    /* Graphics controller registers */
    static const u8 gfx[9] = {0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0F,0xFF};
    for (u8 i = 0; i < 9; i++) {
        outb(0x3CE, i);
        outb(0x3CF, gfx[i]);
    }

    /* Attribute controller registers */
    static const u8 att[21] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
        0x41,0x00,0x0F,0x00,0x00
    };
    (void)inb(0x3DA); /* reset the flip-flop, or the next writes go to the wrong register */
    for (u8 i = 0; i < 21; i++) {
        outb(0x3C0, i);
        outb(0x3C0, att[i]);
    }
    (void)inb(0x3DA);
    outb(0x3C0, 0x20); /* okay NOW actually turn the screen on */
}

/* Manually program the first 16 DAC palette entries to sane VGA colors.
 * We can't trust whatever the BIOS left behind after we just bulldozed
 * half its register state above, so we set this ourselves rather than
 * pray. VGA DAC values are 0-63 (6-bit), not 0-255, because of course
 * they are.
 *
 * Mode 13h's hardware has always had a full 256-entry palette; we were
 * just only ever using the first 16 of it. This programs the other 240
 * too, in the same layout xterm's 256-color terminal palette uses: a
 * 6x6x6 RGB color cube (indices 16-231) followed by a 24-step grayscale
 * ramp (232-255). Nothing here needs to change how existing COL_* pixel
 * values look -- they're still indices 0-15, untouched -- this just
 * fills in real colors behind the 240 palette slots that used to be
 * whatever garbage the hardware happened to power on with. */
static inline void vga_set_standard_palette(void) {
    static const u8 pal[16][3] = {
        {0,0,0},    {0,0,42},   {0,42,0},   {0,42,42},
        {42,0,0},   {42,0,42},  {42,21,0},  {42,42,42},
        {21,21,21}, {21,21,63}, {21,63,21}, {21,63,63},
        {63,21,21}, {63,21,63}, {63,63,21}, {63,63,63},
    };
    outb(0x3C8, 0); /* start writing at palette index 0; each 3-byte
                      * write auto-advances to the next index, so the
                      * cube and grayscale writes below just continue
                      * on from wherever this loop left off */
    for (int i = 0; i < 16; i++) {
        outb(0x3C9, pal[i][0]);
        outb(0x3C9, pal[i][1]);
        outb(0x3C9, pal[i][2]);
    }

    /* indices 16-231: 6x6x6 RGB cube. Levels aren't linear 0/13/26/.../63
     * -- biased slightly toward the bright end, since banding is more
     * visible there than in the shadows. */
    static const u8 cube_level[6] = {0, 12, 24, 37, 49, 63};
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                outb(0x3C9, cube_level[r]);
                outb(0x3C9, cube_level[g]);
                outb(0x3C9, cube_level[b]);
            }
        }
    }

    /* indices 232-255: 24-step grayscale ramp, filling in the smooth
     * gradient the blocky cube can't really cover. */
    for (int i = 0; i < 24; i++) {
        u8 v = (u8)(2 + (i * 61) / 23); /* ~2..63 */
        outb(0x3C9, v);
        outb(0x3C9, v);
        outb(0x3C9, v);
    }
}

/* Palette index for a cube color, r/g/b each 0-5 -- matches the layout
 * vga_set_standard_palette() programmed into indices 16-231. */
static inline u8 col_cube(int r, int g, int b) {
    return (u8)(16 + 36 * r + 6 * g + b);
}
/* Palette index for a grayscale step, 0 (near-black) - 23 (near-white). */
static inline u8 col_gray(int step) {
    return (u8)(232 + step);
}

static inline void vga_putpixel(int x, int y, u8 color) {
    if (x < 0 || y < 0 || x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
    VGA_MEMORY[y * VGA_WIDTH + x] = color;
}

/* ---------------------------------------------------------------------
 * Off-screen back buffer + double buffering.
 *
 * Redrawing the whole desktop straight into visible VGA memory every
 * single frame looks like absolute garbage -- flickery, tearing, the
 * works -- especially once windows can open/close/drag/minimize at any
 * moment from a mouse click. So every drawing function below targets
 * `backbuf` (just a chunk of RAM, invisible to the screen) and
 * vga_present() blasts the finished frame over to real VGA memory in one
 * shot at the very end of each loop. Draw all you want in private, only
 * show the final result.
 * ------------------------------------------------------------------- */
static u8 backbuf[VGA_WIDTH * VGA_HEIGHT];

static inline void bb_putpixel(int x, int y, u8 color) {
    if ((unsigned)x >= VGA_WIDTH || (unsigned)y >= VGA_HEIGHT) return;
    backbuf[y * VGA_WIDTH + x] = color;
}

static inline void bb_fillrect(int x, int y, int w, int h, u8 color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > VGA_WIDTH)  w = VGA_WIDTH - x;
    if (y + h > VGA_HEIGHT) h = VGA_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        u8 *row = &backbuf[(y + j) * VGA_WIDTH + x];
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

static inline void bb_rect(int x, int y, int w, int h, u8 color) {
    for (int i = 0; i < w; i++) { bb_putpixel(x+i, y, color); bb_putpixel(x+i, y+h-1, color); }
    for (int j = 0; j < h; j++) { bb_putpixel(x, y+j, color); bb_putpixel(x+w-1, y+j, color); }
}

static inline void vga_present(void) {
    /* One tight little copy loop, moving 4 bytes at a time instead of 1,
     * because both buffers are exactly VGA_WIDTH*VGA_HEIGHT and nobody's
     * got time for per-pixel bounds checks on a straight memcpy. */
    u32 *src = (u32*)backbuf;
    u32 *dst = (u32*)VGA_MEMORY;
    for (int i = 0; i < (VGA_WIDTH * VGA_HEIGHT) / 4; i++) dst[i] = src[i];
}

/* Direct-to-screen variants -- these still exist but nothing in the
 * kernel actually uses them anymore now that everything goes through the
 * back buffer above. Kept around in case some future feature needs to
 * punch a pixel straight onto the glass without the whole double-buffer
 * dance. */
static inline void vga_fillrect(int x, int y, int w, int h, u8 color) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            vga_putpixel(x + i, y + j, color);
}

static inline void vga_rect(int x, int y, int w, int h, u8 color) {
    for (int i = 0; i < w; i++) { vga_putpixel(x+i, y, color); vga_putpixel(x+i, y+h-1, color); }
    for (int j = 0; j < h; j++) { vga_putpixel(x, y+j, color); vga_putpixel(x+w-1, y+j, color); }
}

static inline void vga_clear(u8 color) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) VGA_MEMORY[i] = color;
}

/* Standard 16-color VGA palette indices (works fine even in our 256-color
 * mode since the default DAC palette's first 16 entries match the old
 * EGA colors -- one of the rare cases where legacy compatibility
 * actually works in our favor) */
#define COL_BLACK       0
#define COL_BLUE        1
#define COL_GREEN       2
#define COL_CYAN        3
#define COL_RED         4
#define COL_MAGENTA     5
#define COL_BROWN       6
#define COL_LGRAY       7
#define COL_DGRAY       8
#define COL_LBLUE       9
#define COL_LGREEN      10
#define COL_LCYAN       11
#define COL_LRED        12
#define COL_LMAGENTA    13
#define COL_YELLOW      14
#define COL_WHITE       15

#endif
