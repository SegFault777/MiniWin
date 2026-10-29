/* SPDX-License-Identifier: GPL-3.0-only */
/* Kerning (kernel/font.h, 1.1-alpha-12): properties that must hold for EVERY pair of printable ASCII letters, checked against the
 * real kernel code (font.h is included as it is; only the screen is stubbed out).
 *   - the advance never exceeds the fixed cell, so kerned text is never wider than fixed text;
 *   - a pair never moves by more than KERN_MAX, and never apart (kern <= 0);
 *   - after kerning, no pixel row in which BOTH letters have ink has less than one empty column between them (no two letters touch);
 *   - a few well-known pairs close up (A V, T o, L T) and a few never do (H H);
 *   - kerned width of a string equals what ko_string_width-style stepping gives, and is never larger than the fixed width. */
#include <stdio.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t i32;
#define IO_H
#define VGA_H
static void bb_putpixel(int x, int y, u32 c) { (void)x; (void)y; (void)c; }
#include "../../kernel/font_latin_data.h"
#include "../../kernel/font.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    for (int a = 32; a < 127; a++) {
        int adv = font_adv((char)a);
        CHECK(adv >= 2 && adv <= FONT_CELL, "adv('%c') = %d", a, adv);
        for (int b = 32; b < 127; b++) {
            int k = font_kern((char)a, (char)b);
            CHECK(k <= 0 && k >= -KERN_MAX, "kern('%c','%c') = %d out of range", a, b, k);
            const u16 *ga = fk_glyph((char)a), *gb = fk_glyph((char)b);
            int la = fk_ink_l(ga), lb = fk_ink_l(gb);
            if (la == 99 || lb == 99) continue;
            int pa = fk_pad(ga), pb = fk_pad(gb);
            for (int r = 0; r < FONT_CELL; r++) {
                if (!ga[r] || !gb[r]) continue;
                int a_right = pa + fk_last(ga[r]) - la;                     /* where a's last pixel of this row lands (a's origin = 0) */
                int b_left = font_adv((char)a) + k + pb + fk_first(gb[r]) - lb;
                CHECK(b_left - a_right - 1 >= 1, "'%c''%c' touch in row %d (empty columns %d)", a, b, r, b_left - a_right - 1);
            }
        }
    }
    CHECK(font_kern('A', 'V') < 0, "AV should close up");
    CHECK(font_kern('T', 'o') < 0, "To should close up");
    CHECK(font_kern('L', 'T') < 0, "LT should close up");
    CHECK(font_kern('H', 'H') == 0, "HH must not move");
    CHECK(font_adv('i') < font_adv('m'), "i is narrower than m");
    CHECK(font_adv(' ') == 4, "space is 4 wide");

    const char *s[] = { "SETTING.MWP OPENED", "AVATAR Town", "Mount Unmount Install", "The quick brown fox", 0 };
    for (int i = 0; s[i]; i++) {
        int w = 0, n = 0;
        for (const char *p = s[i]; *p; p++, n++) w += font_adv(*p) + (p[1] ? font_kern(*p, p[1]) : 0);
        CHECK(w <= n * FONT_CELL, "kerned \"%s\" is %d px, fixed is %d", s[i], w, n * FONT_CELL);
    }
    printf("kerning: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
