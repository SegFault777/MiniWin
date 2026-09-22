#ifndef FONT_H
#define FONT_H
#include "vga.h"
#include "font_latin_data.h"

/* 11x11 bitmap font, Galmuri11-flavored (see third_party/galmuri-font/
 * and tools/gen_latin_font.py) -- covering all of printable ASCII,
 * uppercase AND lowercase both drawn as their own real glyphs now
 * (the old 8x8 font's lowercase table was a separate hand-drawn
 * afterthought bolted onto an uppercase-only base; Galmuri11 just
 * draws both properly because a real font was never missing lowercase
 * to begin with). We didn't suffer for this one -- a font family that
 * already existed suffered for us, and we're deeply grateful.
 *
 * FONT_CELL is the fixed advance width AND height every glyph takes
 * up on screen -- this renderer has no per-character kerning or
 * variable advance, exactly like the 8x8 version it replaces, just
 * bigger. Any code doing manual `x += 8` character-spacing math
 * elsewhere in the kernel needs updating to FONT_CELL; see the UI
 * scale-up pass for that sweep. */
#define FONT_CELL 11

static inline char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

static inline void font_draw_char(int x, int y, char c, u32 color) {
    if (c < FONT_LATIN_FIRST_CP || c >= FONT_LATIN_FIRST_CP + FONT_LATIN_COUNT) {
        return; /* don't know this character, and not gonna guess */
    }
    const u16 *glyph = font_latin[(int)(unsigned char)c - FONT_LATIN_FIRST_CP];

    for (int row = 0; row < FONT_CELL; row++) {
        u16 bits = glyph[row];
        for (int col = 0; col < FONT_CELL; col++) {
            if (bits & (0x8000 >> col)) {
                bb_putpixel(x + col, y + row, color);
            }
        }
    }
}

static inline void font_draw_string(int x, int y, const char *s, u32 color) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += FONT_CELL + 1; s++; continue; }
        font_draw_char(cx, y, *s, color);
        cx += FONT_CELL;
        s++;
    }
}

/* Rotates a glyph 90 degrees on the way to the screen, purely by
 * relabeling which bit goes where -- no actual spinning involved, we
 * just read the same 11x11 bitmap sideways. This is exactly the kind of
 * "vertical logo" trick Windows 95 pulled off in its Start Menu, and
 * we're stealing it shamelessly rather than shipping a whole second
 * font just so a handful of letters can stand up straight.
 * (x,y) is the top-left corner of the resulting FONT_CELLxFONT_CELL
 * box, same as font_draw_char -- it just happens to contain a sideways
 * letter. Uppercased first (via to_upper) since the vertical taskbar
 * strip is the one spot in the UI that still wants shouty caps. */
static inline void font_draw_char_vertical(int x, int y, char c, u32 color) {
    char u = to_upper(c);
    if (u < FONT_LATIN_FIRST_CP || u >= FONT_LATIN_FIRST_CP + FONT_LATIN_COUNT) return;
    const u16 *glyph = font_latin[(int)(unsigned char)u - FONT_LATIN_FIRST_CP];

    for (int row = 0; row < FONT_CELL; row++) {
        u16 bits = glyph[row];
        for (int col = 0; col < FONT_CELL; col++) {
            if (bits & (0x8000 >> col)) {
                bb_putpixel(x + row, y + (FONT_CELL - 1 - col), color);
            }
        }
    }
}

/* Stacks rotated characters upward from (x, y_bottom), so the string
 * reads bottom-to-top -- perfect for a thin vertical strip along the
 * left edge of a popup menu, or for silently judging anyone still
 * running this OS at 640x400. */
static inline void font_draw_string_vertical(int x, int y_bottom, const char *s, u32 color) {
    int cy = y_bottom;
    while (*s) {
        font_draw_char_vertical(x, cy, *s, color);
        cy -= FONT_CELL;
        s++;
    }
}

#endif
