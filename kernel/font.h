/* SPDX-License-Identifier: GPL-3.0-only */
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
 * up on screen -- font_draw_string() has no per-character kerning or
 * variable advance, exactly like the 8x8 version it replaces, just
 * bigger (the kerned path for UI labels is further down: font_kern()). Any code doing manual `x += 8` character-spacing math
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

/* ============================================================
 * KERNING (1.1-alpha-12) -- Settings > Text > Kerning.
 *
 * The font is a fixed 11x11 cell, so "AV" sits in two cells with a hole between the shoulders of the A and the V, and an "i" sits
 * in a cell three times as wide as it is. Kerning fixes both without a second font and without a table:
 *   - every glyph's INK (the first and last column that has a pixel in it) is measured straight from its bitmap, and the kerned
 *     advance is ink width + 1 empty column (never wider than the cell, so kerned text is never wider than fixed text);
 *   - for a pair of letters, each of the 11 pixel rows says how far the right edge of the first letter is from the left edge of the
 *     second; the tightest row decides how much the pair may slide together while keeping one empty column there. That is how A/V,
 *     T/o, L/T, P/A close up and how l/l or H/H do not move at all -- the same idea a type designer's kerning table encodes by
 *     hand, computed here from the pixels (11 clz/ctz per pair, nothing stored). A pair never moves more than KERN_MAX pixels.
 * Only the Latin letters of UI labels are kerned (ko_draw_mixed_string / ko_string_width): Hangul syllables are full-cell
 * squares, and the shell, Notepad and every other fixed-grid surface keep using font_draw_string, so columns still line up.
 * `font_kerning` is the switch (Settings > Text); off, ko_draw_mixed_string is exactly what it was in alpha-11.
 * ============================================================ */
#define KERN_MAX 2
static int font_kerning = 1;

static inline const u16 *fk_glyph(char c) {
    int i = (int)(unsigned char)c - FONT_LATIN_FIRST_CP;
    return (i < 0 || i >= FONT_LATIN_COUNT) ? 0 : font_latin[i];
}
static inline int fk_first(u16 b) { return b ? __builtin_clz((u32)b << 16) : 99; }     /* column (0 = leftmost) of the first pixel in a row */
static inline int fk_last(u16 b)  { return b ? 15 - __builtin_ctz((u32)b) : -99; }     /* column of the last pixel in a row */
static int fk_ink_l(const u16 *g) { int m = 99; for (int r = 0; r < FONT_CELL; r++) { int l = fk_first(g[r]); if (l < m) m = l; } return m; }
static int fk_ink_r(const u16 *g) { int m = -99; for (int r = 0; r < FONT_CELL; r++) { int l = fk_last(g[r]); if (l > m) m = l; } return m; }

/* A glyph only one or two columns wide (. , : ; i l) gets one extra empty column on EACH side: without it a full stop sits right
 * up against the curve of the letter before it and reads as part of that letter ("SETTING.MWP" looked like "SETTINGMWP"). */
static int fk_pad(const u16 *g) { int l = fk_ink_l(g); return (l != 99 && fk_ink_r(g) - l + 1 <= 2) ? 1 : 0; }

/* how far to the right the NEXT character starts after `c` (kerned) */
static int font_adv(char c) {
    const u16 *g = fk_glyph(c);
    if (!g) return FONT_CELL;
    int l = fk_ink_l(g);
    if (l == 99) return c == ' ' ? 4 : FONT_CELL;                     /* a blank glyph: only the space is narrow */
    int w = fk_ink_r(g) - l + 2 + 2 * fk_pad(g);                      /* ink columns + 1 empty column (+ the narrow glyph's side columns) */
    return w > FONT_CELL ? FONT_CELL : w;
}
/* the extra shift (0 .. -KERN_MAX) between `a` and a following `b` */
static int font_kern(char a, char b) {
    const u16 *ga = fk_glyph(a), *gb = fk_glyph(b);
    if (!ga || !gb) return 0;
    int la = fk_ink_l(ga), lb = fk_ink_l(gb);
    if (la == 99 || lb == 99) return 0;                               /* a space or blank: nothing to tuck under */
    int adv = font_adv(a), pa = fk_pad(ga), pb = fk_pad(gb), best = 99;
    for (int r = 0; r < FONT_CELL; r++) {
        if (!ga[r] || !gb[r]) continue;                               /* only rows where BOTH letters have ink can collide */
        /* ...but a pixel also touches the pixels one row above and below it: a full stop tucked beside the last pixel row of a "G"
         * would otherwise slide up against the G's right-hand stroke (it ended up looking like part of the letter). So each side's
         * edge in this row is the widest/narrowest of rows r-1..r+1. */
        int ar = -99, bl = 99;
        for (int q = r - 1; q <= r + 1; q++) {
            if (q < 0 || q >= FONT_CELL) continue;
            int e = fk_last(ga[q]), f = fk_first(gb[q]);
            if (e > ar) ar = e;
            if (f < bl) bl = f;
        }
        int gap = (adv + pb + bl - lb) - (pa + ar - la) - 1;          /* empty columns between them around this row */
        if (gap < best) best = gap;
    }
    int want = (pa | pb) ? 2 : 1;                                     /* the tightest row keeps 1 empty column -- 2 next to a . , : i l, which read as part of the neighbour at 1 */
    if (best == 99 || best <= want) return 0;                         /* no shared row, or already as tight as wanted */
    int k = best - want;
    return -(k > KERN_MAX ? KERN_MAX : k);
}
/* one glyph with its ink starting exactly at x (the kerned advance counts from the ink, not from the cell) */
static inline void font_draw_char_k(int x, int y, char c, u32 color) {
    const u16 *g = fk_glyph(c);
    if (!g) return;
    int l = fk_ink_l(g);
    font_draw_char(x + fk_pad(g) - (l == 99 ? 0 : l), y, c, color);
}

/* Plain-ASCII strings in the kerned style, for the many labels that are not Hangul-capable (taskbar, icon captions, title bars, buttons,
 * status bars): font_width_k() is how wide font_draw_k() paints it. With Settings > Text > Kerning off both are exactly the fixed-cell
 * font_draw_string() and FONT_CELL * length. */
static int font_width_k(const char *s) {
    int w = 0;
    for (; *s; s++) w += font_kerning ? font_adv(*s) + (s[1] ? font_kern(*s, s[1]) : 0) : FONT_CELL;
    return w;
}
static inline void font_draw_string(int x, int y, const char *s, u32 color);
static void font_draw_k(int x, int y, const char *s, u32 color) {
    if (!font_kerning) { font_draw_string(x, y, s, color); return; }
    for (; *s; s++) { font_draw_char_k(x, y, *s, color); x += font_adv(*s) + (s[1] ? font_kern(*s, s[1]) : 0); }
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
