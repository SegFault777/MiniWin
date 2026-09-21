#ifndef VGA_H
#define VGA_H
#include "io.h"
#include "serial.h"

#define VGA_WIDTH  640
#define VGA_HEIGHT 480

/* The video mode itself (a real 640x480, 32-bit-per-pixel, direct-color
 * VBE mode -- true color, not a 256-entry palette) is found and set by
 * the bootloader's second stage in real mode -- BIOS calls fundamentally
 * can't be made from protected mode without extra VM86/real-mode-shim
 * machinery this kernel doesn't have, so mode-setting has to happen
 * before boot/stage2.asm ever switches to protected mode. Unlike the
 * old approach (a hardcoded VESA mode NUMBER for 640x400x8bpp, which
 * happens to be a stable convention only at that specific resolution/
 * depth), there's no equivalent stable number for a 32bpp truecolor
 * mode -- different VBE implementations assign it differently -- so
 * stage 2 actually walks the BIOS's own mode list looking for one that
 * matches, and leaves whichever mode it found sitting in the Mode Info
 * Block at physical address 0x9000 for the kernel to read here.
 *
 * PhysBasePtr (the LFB's physical base address) lives 40 bytes into
 * that block, BytesPerScanLine (the real hardware's row pitch, which
 * isn't guaranteed to equal VGA_WIDTH*4) at offset 16, and the
 * Red/Green/BlueFieldPosition bytes (how far to shift each 8-bit color
 * component into the final 32-bit pixel value) at offsets 32/34/36.
 * Since this kernel runs with paging disabled, "read a physical
 * address" is just "dereference a pointer" -- no mapping step needed. */
static u8 *vga_lfb_ptr = 0;
static u32 vga_pitch = 0;
static u8  vga_red_shift = 16, vga_green_shift = 8, vga_blue_shift = 0;
#define VGA_MEMORY vga_lfb_ptr

/* The backbuffer this whole kernel draws into (see further down) is
 * 640*480*4 = 1,228,800 bytes -- far too big to live in this kernel's
 * usual low-memory footprint the way the old 8bpp backbuf (250KB) just
 * barely could. Everything else this kernel owns (its own code/data,
 * the stack) stays under the 640KB conventional-memory line on
 * purpose; a 1.2MB array simply doesn't fit there twice over. Placed
 * instead at a fixed physical address well above 1MB, using the exact
 * same "paging is off, so a raw address IS a pointer" trick the LFB
 * pointer above already relies on -- 4MB is comfortably clear of this
 * kernel's own footprint (which tops out well under 1MB) and of any
 * BIOS/legacy reserved regions, assuming the machine has enough RAM to
 * begin with. That assumption is checked, not trusted blindly -- see
 * vga_verify_memory_safe() below, which the kernel calls before this
 * pointer is ever written through. */
#define VGA_BACKBUF_PHYS_ADDR 0x400000u
#define VGA_BACKBUF_BYTES ((u32)VGA_WIDTH * VGA_HEIGHT * 4)
/* Comfortable safety margin above the backbuffer's own footprint, for
 * the kernel's own code/data/stack (all under 1MB) plus headroom --
 * not a tight fit, just "enough that a machine failing this check is
 * unambiguously too small, not a borderline judgment call." */
#define VGA_MIN_RAM_KB ((VGA_BACKBUF_PHYS_ADDR / 1024) + (VGA_BACKBUF_BYTES / 1024) + 4096u)

static inline void vga_init_display(void) {
    vga_lfb_ptr = *(u8 **)(0x9000 + 40);
    vga_pitch = *(u32 *)(0x9000 + 16) & 0xFFFF; /* field is a u16; masked
                                                 * defensively in case
                                                 * whatever garbage sits
                                                 * past it in the block
                                                 * ever got misread as
                                                 * part of a wider access */
    if (vga_pitch == 0) vga_pitch = (u32)VGA_WIDTH * 4; /* paranoia fallback, shouldn't trigger */
    vga_red_shift   = *(u8 *)(0x9000 + 32);
    vga_green_shift = *(u8 *)(0x9000 + 34);
    vga_blue_shift  = *(u8 *)(0x9000 + 36);
}

/* Every real 32bpp direct-color VBE implementation this kernel has
 * ever been tested against -- QEMU/Bochs VBE, and every other emulator
 * and real card with any real-world install base -- lays out red at
 * bit 16, green at bit 8, blue at bit 0 (the universal 0x00RRGGBB
 * convention, the same one CSS hex colors and virtually every other
 * 24-bit color format in computing uses). The field positions ARE read
 * from the hardware above rather than just assumed, though, so this
 * function can tell the difference between "matches the convention
 * every COL_* constant below was written assuming" and "doesn't" --
 * and refuses to guess wrong silently if it's ever the latter. */
static inline int vga_color_layout_is_standard(void) {
    return vga_red_shift == 16 && vga_green_shift == 8 && vga_blue_shift == 0;
}

/* Checks the memory-size figure boot/stage2.asm's INT15h/E801h probe
 * left at physical address 0x9200 (in KB) against what the truecolor
 * backbuffer at VGA_BACKBUF_PHYS_ADDR actually needs. Returns 1 if
 * it's safe to proceed, 0 if the probe failed outright (stored as 0)
 * or reported less than VGA_MIN_RAM_KB -- in which case writing
 * through the backbuffer pointer could scribble over real, actively-used
 * memory (or nothing mapped at all) instead of harmless free RAM.
 * Called once, early in kmain(), before anything ever touches
 * backbuf[]. */
static inline int vga_verify_memory_safe(void) {
    u32 total_kb = *(u32 *)0x9200;
    return total_kb >= VGA_MIN_RAM_KB;
}

static inline void vga_putpixel(int x, int y, u32 color) {
    if (x < 0 || y < 0 || x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
    u32 *row = (u32 *)(VGA_MEMORY + (u32)y * vga_pitch);
    row[x] = color;
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
 *
 * backbuf itself is a POINTER to the fixed physical address above, not
 * a plain static array the way the old 8bpp version was -- see
 * VGA_BACKBUF_PHYS_ADDR's own comment for why a buffer this size can't
 * live in the kernel's normal .bss. Every pixel is a full 32-bit direct
 * color value now (built via the RGB() macro below), not a palette
 * index into a 256-color lookup table.
 * ------------------------------------------------------------------- */
#define backbuf ((u32 *)VGA_BACKBUF_PHYS_ADDR)

/* Packs 8-bit red/green/blue components into one direct-color pixel
 * value, honoring whatever field positions vga_init_display() read
 * from the hardware -- though in practice, thanks to
 * vga_color_layout_is_standard()'s check at boot, this only ever
 * actually runs with the universal 16/8/0 layout every COL_* constant
 * below already assumes. Kept as a real shift-based function rather
 * than inlining the assumption directly, so the one place this kernel
 * ever builds an arbitrary (non-constant) color has a single correct
 * way to do it. */
static inline u32 RGB(u8 r, u8 g, u8 b) {
    return ((u32)r << vga_red_shift) | ((u32)g << vga_green_shift) | ((u32)b << vga_blue_shift);
}

static inline void bb_putpixel(int x, int y, u32 color) {
    if ((unsigned)x >= VGA_WIDTH || (unsigned)y >= VGA_HEIGHT) return;
    backbuf[y * VGA_WIDTH + x] = color;
}

static inline void bb_fillrect(int x, int y, int w, int h, u32 color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > VGA_WIDTH)  w = VGA_WIDTH - x;
    if (y + h > VGA_HEIGHT) h = VGA_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        u32 *row = &backbuf[(y + j) * VGA_WIDTH + x];
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

static inline void bb_rect(int x, int y, int w, int h, u32 color) {
    for (int i = 0; i < w; i++) { bb_putpixel(x+i, y, color); bb_putpixel(x+i, y+h-1, color); }
    for (int j = 0; j < h; j++) { bb_putpixel(x, y+j, color); bb_putpixel(x+w-1, y+j, color); }
}

static inline void vga_present(void) {
    /* Row-by-row instead of one giant blit, since the real hardware's
     * scanline pitch (vga_pitch, in BYTES) isn't guaranteed to equal
     * VGA_WIDTH*4 -- backbuf itself stays tightly packed (it's our own
     * buffer, our own layout choice), but the real framebuffer's rows
     * have to be addressed by whatever pitch the BIOS actually
     * reported. Copies a whole scanline's worth of 32-bit pixels per
     * row -- vga_pitch is in bytes, so dividing by 4 gives the pixel
     * count to copy (padding bytes some cards add past VGA_WIDTH, if
     * any, are simply left untouched, same as before). */
    u32 pixels_per_row = vga_pitch / 4;
    for (int y = 0; y < VGA_HEIGHT; y++) {
        const u32 *src = &backbuf[(u32)y * VGA_WIDTH];
        u32 *dst = (u32 *)(VGA_MEMORY + (u32)y * vga_pitch);
        for (u32 i = 0; i < pixels_per_row && i < (u32)VGA_WIDTH; i++) dst[i] = src[i];
    }
}

/* Direct-to-screen variants -- these still exist but nothing in the
 * kernel actually uses them anymore now that everything goes through the
 * back buffer above. Kept around in case some future feature needs to
 * punch a pixel straight onto the glass without the whole double-buffer
 * dance. */
static inline void vga_fillrect(int x, int y, int w, int h, u32 color) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            vga_putpixel(x + i, y + j, color);
}

static inline void vga_rect(int x, int y, int w, int h, u32 color) {
    for (int i = 0; i < w; i++) { vga_putpixel(x+i, y, color); vga_putpixel(x+i, y+h-1, color); }
    for (int j = 0; j < h; j++) { vga_putpixel(x, y+j, color); vga_putpixel(x+w-1, y+j, color); }
}

static inline void vga_clear(u32 color) {
    for (int y = 0; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            vga_putpixel(x, y, color);
}

/* True 24-bit RGB values now, not palette indices -- the exact same 16
 * colors the old DAC-programmed EGA/VGA palette used (the universal
 * "Windows 16-color" / ANSI-terminal 16-color RGB values, the same
 * numbers essentially every reference table for this classic palette
 * agrees on), just expressed directly instead of through a 4-bit
 * lookup. Every one of the 150+ places in kernel.c that reference
 * COL_BLACK, COL_WHITE, and so on didn't need to change at all for
 * this cutover -- they were always symbolic names, never raw numbers,
 * so only the definitions here and the underlying draw primitives'
 * types (u8 -> u32) had to move. */
#define COL_BLACK       0x000000u
#define COL_BLUE        0x0000AAu
#define COL_GREEN       0x00AA00u
#define COL_CYAN        0x00AAAAu
#define COL_RED         0xAA0000u
#define COL_MAGENTA     0xAA00AAu
#define COL_BROWN       0xAA5500u
#define COL_LGRAY       0xAAAAAAu
#define COL_DGRAY       0x555555u
#define COL_LBLUE       0x5555FFu
#define COL_LGREEN      0x55FF55u
#define COL_LCYAN       0x55FFFFu
#define COL_LRED        0xFF5555u
#define COL_LMAGENTA    0xFF55FFu
#define COL_YELLOW      0xFFFF55u
#define COL_WHITE       0xFFFFFFu

#endif
