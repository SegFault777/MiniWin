#ifndef VGA_H
#define VGA_H
#include "io.h"

#define VGA_WIDTH  640
#define VGA_HEIGHT 400

/* The video mode itself (VBE mode 0100h, 640x400x256) is set by the
 * bootloader in real mode -- BIOS calls fundamentally can't be made
 * from protected mode without extra VM86/real-mode-shim machinery this
 * kernel doesn't have, so unlike the old mode-13h days (which could be
 * bit-banged directly via VGA registers from anywhere, including here),
 * VBE mode-setting has to happen before boot.asm ever switches to
 * protected mode -- see boot/boot.asm.
 *
 * All that's left for the kernel to do is find out where the BIOS
 * actually put the linear framebuffer: boot.asm stashed the VBE mode
 * info block at physical address 0x9000, and PhysBasePtr (the LFB's
 * physical base address) lives 40 bytes into it, with BytesPerScanLine
 * (the real hardware's row pitch, which isn't guaranteed to equal
 * VGA_WIDTH the way it always did in mode 13h) at offset 16. Since this
 * kernel runs with paging disabled, "read a physical address" is just
 * "dereference a pointer" -- no mapping step needed. */
static u8 *vga_lfb_ptr = 0;
static u32 vga_pitch = 0;
#define VGA_MEMORY vga_lfb_ptr

static inline void vga_init_display(void) {
    vga_lfb_ptr = *(u8 **)(0x9000 + 40);
    vga_pitch = *(u16 *)(0x9000 + 16);
    if (vga_pitch == 0) vga_pitch = VGA_WIDTH; /* paranoia fallback, shouldn't trigger */
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
    VGA_MEMORY[y * vga_pitch + x] = color;
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
    /* Row-by-row instead of one giant blit, since the real hardware's
     * scanline pitch (vga_pitch) isn't guaranteed to equal VGA_WIDTH the
     * way it always did in mode 13h -- backbuf itself stays tightly
     * packed (it's our own buffer, our own layout choice), but the real
     * framebuffer's rows have to be addressed by whatever pitch the
     * BIOS actually reported. Still 4 bytes at a time within each row. */
    for (int y = 0; y < VGA_HEIGHT; y++) {
        u32 *src = (u32 *)(backbuf + (u32)y * VGA_WIDTH);
        u32 *dst = (u32 *)(VGA_MEMORY + (u32)y * vga_pitch);
        for (int i = 0; i < VGA_WIDTH / 4; i++) dst[i] = src[i];
    }
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
    for (int y = 0; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA_MEMORY[y * vga_pitch + x] = color;
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
