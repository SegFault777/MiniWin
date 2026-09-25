#ifndef FONT_KO_H
#define FONT_KO_H
#include "io.h"
#include "vga.h"
#include "font.h"
#include "font_ko_data.h"

/* ============================================================
 * Modern Hangul syllable composition
 *
 * Some genius at the Unicode Consortium figured out that every possible
 * modern Hangul syllable can be reached with one dumb little formula
 * instead of storing 11172 arbitrary codepoints by hand. Bless them.
 *   codepoint = 0xAC00 + (initial*21 + medial)*28 + final
 * where:
 *   initial: 0-18  (19 leading consonants)
 *   medial:  0-20  (21 vowels)
 *   final:   0-27  (28 trailing consonants, 0 = no batchim)
 * ============================================================ */

static inline int ko_compose_codepoint(int initial, int medial, int final_) {
    if (initial < 0 || initial > 18) return -1;
    if (medial < 0 || medial > 20) return -1;
    if (final_ < 0 || final_ > 27) return -1;
    return KO_SYLLABLE_BASE + (initial * 21 + medial) * 28 + final_;
}

static inline const u16 *ko_glyph_for_codepoint(int cp) {
    if (cp >= KO_SYLLABLE_BASE && cp < KO_SYLLABLE_BASE + KO_SYLLABLE_COUNT) {
        return font8x8_ko[cp - KO_SYLLABLE_BASE];
    }
    if (cp >= KO_JAMO_BASE && cp < KO_JAMO_BASE + KO_JAMO_COUNT) {
        return font8x8_ko_jamo[cp - KO_JAMO_BASE];
    }
    return 0;
}

static inline void ko_font_draw_glyph(int x, int y, const u16 *glyph, u32 color) {
    if (!glyph) {
        /* We got asked to draw a codepoint we've never heard of. Rather
         * than silently drawing nothing (which just looks like a bug
         * someone will spend an hour chasing), slap down an obvious
         * little hollow box so it screams "something's missing here." */
        bb_rect(x, y, FONT_CELL - 1, FONT_CELL - 1, color);
        return;
    }
    for (int row = 0; row < FONT_CELL; row++) {
        u16 bits = glyph[row];
        for (int col = 0; col < FONT_CELL; col++) {
            if (bits & (0x8000 >> col)) {
                bb_putpixel(x + col, y + row, color);
            }
        }
    }
}

static inline void ko_font_draw_codepoint(int x, int y, int cp, u32 color) {
    ko_font_draw_glyph(x, y, ko_glyph_for_codepoint(cp), color);
}

/* ============================================================
 * Minimal UTF-8 helpers. text_buf holds plain ASCII (1 byte) for Latin
 * junk and honest-to-god standard 3-byte UTF-8 for Hangul syllables/jamo
 * (they all live in the U+0800-U+FFFF range, conveniently). This means
 * saved files are REAL UTF-8 text, not some made-up encoding only our
 * kernel understands -- open one in literally any text editor and it'll
 * just work. We checked. It does.
 * ============================================================ */

/* How many bytes does the character starting at `lead` take up? */
static inline int ko_utf8_char_len(unsigned char lead) {
    if ((lead & 0xF0) == 0xE0) return 3; /* 1110xxxx: one of our fancy 3-byte guys */
    return 1;
}

/* Decodes the 3-byte sequence at s[0..2]. Caller better have already
 * confirmed via ko_utf8_char_len that there really are 3 bytes there --
 * we're not checking again, we trust you. Don't make that a mistake. */
static inline int ko_utf8_decode3(const char *s) {
    unsigned char b0 = (unsigned char)s[0];
    unsigned char b1 = (unsigned char)s[1];
    unsigned char b2 = (unsigned char)s[2];
    return ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
}

/* How many bytes was the LAST character in buf[0..len-1]? Backspace needs
 * this so it deletes a whole Hangul syllable in one go instead of hacking
 * off one byte of a 3-byte sequence and leaving a mangled UTF-8 corpse
 * behind. Nobody wants that. */
static inline int ko_utf8_last_char_len(const char *buf, u32 len) {
    if (len >= 3 &&
        ((unsigned char)buf[len - 1] & 0xC0) == 0x80 &&
        ((unsigned char)buf[len - 2] & 0xC0) == 0x80 &&
        ((unsigned char)buf[len - 3] & 0xF0) == 0xE0) {
        return 3;
    }
    return 1;
}

/* ============================================================
 * Mixed-string drawing -- for any UI text that's plain ASCII except for
 * our 3-byte-UTF-8 Hangul syllables/jamo sprinkled in (which is exactly
 * what SETTING.MWP's language switch produces: the same status bar,
 * menus, and dialogs, just occasionally speaking Korean instead of
 * English). This is the identical byte-sniffing loop the Notepad text
 * area already runs over whatever the user typed, just factored out so
 * a single-line UI label can use it too without hand-rolling the loop
 * again at every call site. No line-wrapping here on purpose -- UI
 * labels are short and single-line; that's Notepad's problem, not ours.
 * ============================================================ */
static inline void ko_draw_mixed_string(int x, int y, const char *s, u32 color) {
    int cx = x;
    u32 len = 0;
    while (s[len]) len++; /* freestanding kernel, no strlen() lying around */
    u32 i = 0;
    while (i < len) {
        if (s[i] == '\n') { cx = x; y += FONT_CELL + 1; i++; continue; }
        int clen = ko_utf8_char_len((unsigned char)s[i]);
        if (clen == 3 && i + 3 <= len) {
            ko_font_draw_codepoint(cx, y, ko_utf8_decode3(&s[i]), color);
            cx += FONT_CELL;
            i += 3;
        } else {
            font_draw_char(cx, y, s[i], color);
            cx += FONT_CELL;
            i += 1;
        }
    }
}

/* Pixel width ko_draw_mixed_string() would take up -- every character,
 * English or Hangul, is one FONT_CELL-wide cell, so this is just a cell
 * count times FONT_CELL. Handy for centering a label (Yes/No buttons,
 * say) without caring which language is currently active. */
static inline int ko_string_width(const char *s) {
    u32 len = 0;
    while (s[len]) len++;
    u32 i = 0;
    int w = 0;
    while (i < len) {
        int clen = ko_utf8_char_len((unsigned char)s[i]);
        w += FONT_CELL;
        i += (clen == 3 && i + 3 <= len) ? 3 : 1;
    }
    return w;
}

/* ============================================================
 * Standalone jamo lookup -- for showing whatever half-typed consonant or
 * vowel is currently just sitting there before it combines into a real
 * syllable (type just "ㄱ" and nothing else yet, you should still SEE a
 * "ㄱ" on screen, not nothing). Indices line up with the same
 * initial/medial/final numbering used everywhere in hangul_ime.h.
 * ============================================================ */

/* Standalone compatibility-jamo codepoints (U+3131 block) for each of the
 * 19 initial-consonant slots. Fair warning: this is NOT just
 * KO_JAMO_BASE+i, because whoever designed this Unicode block decided to
 * interleave the 10 complex batchim (ㄳㄵㄶㄺㄻㄼㄽㄾㄿㅄ) right in the
 * middle of the 19 simple consonants instead of putting them somewhere
 * sane. So every value here is spelled out by hand instead of computed,
 * because trusting a pattern that isn't actually there is how bugs are
 * born. */
static const int ko_jamo_cp_for_initial[19] = {
    0x3131,0x3132,0x3134,0x3137,0x3138,0x3139,0x3141,0x3142,0x3143,
    0x3145,0x3146,0x3147,0x3148,0x3149,0x314A,0x314B,0x314C,0x314D,0x314E,
};

/* Standalone compatibility-jamo codepoints for the 21 medial-vowel
 * slots. This chunk of the block is actually contiguous (unlike the
 * consonants above), but we're still spelling it out explicitly to match
 * style and so nobody has to trust an assumption at 2am. */
static const int ko_jamo_cp_for_medial[21] = {
    0x314F,0x3150,0x3151,0x3152,0x3153,0x3154,0x3155,0x3156,0x3157,
    0x3158,0x3159,0x315A,0x315B,0x315C,0x315D,0x315E,0x315F,0x3160,
    0x3161,0x3162,0x3163,
};

#endif
