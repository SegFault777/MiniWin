#!/usr/bin/env python3
"""Generates kernel/font_ko_data.h: an 11x11 bitmap glyph table for all
11172 modern Hangul syllables (U+AC00-U+D7A3) plus the 51 standalone
compatibility jamo (U+3131-U+3163), extracted from Galmuri11
(third_party/galmuri-font/, SIL Open Font License 1.1).

This replaces an earlier version of this script that rendered 8x8 glyphs
from the Dalmoori TTF font (third_party/dalmoori-font/) via PIL. Galmuri11
was picked instead for the 11x11 generation because it's a real bitmap
font designed at exactly this cell size (rather than a scalable TTF being
downsampled into one), and because using the same font for both Hangul
and Latin glyphs (see tools/gen_latin_font.py) means the two scripts
actually share a visual style instead of looking like two different UIs
bolted together. Dalmoori and its own LICENSE/NOTICE stay in
third_party/dalmoori-font/ purely as a historical record of the old 8x8
font; nothing in the current build reads it anymore.

Re-run this script (and gen_latin_font.py) together if
third_party/galmuri-font/Galmuri11-subset.bdf is ever updated -- see
tools/bdf_common.py for the shared BDF-parsing and cell-placement logic
both generators use, so Hangul and Latin glyphs agree on exactly where
row 0 and the baseline sit. Every Hangul syllable in this font (unlike
several Latin descenders -- see gen_latin_font.py) fits its full BBX
inside the 11-row cell with zero clipping: Hangul syllable blocks are
designed with no descent below the baseline in the first place, which is
exactly why they're a clean fit for a font whose row 10 IS the baseline.
The generated header is otherwise treated as a build artifact checked
into source for convenience (no Python/BDF toolchain needed to build the
kernel).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bdf_common import parse_bdf, place_glyph, format_row_c_literal, CELL

HERE = os.path.dirname(os.path.abspath(__file__))
BDF_PATH = os.path.join(HERE, "..", "third_party", "galmuri-font", "Galmuri11-subset.bdf")
OUT_PATH = os.path.join(HERE, "..", "kernel", "font_ko_data.h")

BASE = 0xAC00
COUNT = 11172  # 0xD7A3 - 0xAC00 + 1

JAMO_BASE = 0x3131
JAMO_COUNT = 51  # U+3131-U+3163: standalone consonants+vowels, used to
                 # show an in-progress jamo before it combines into a full
                 # syllable (e.g. showing just "ㄱ" while composing).


def emit_table(f, glyphs, base_cp, count, table_name, clip_counter):
    f.write(f"static const u16 {table_name}[{count}][{CELL}] = {{\n")
    for i in range(count):
        cp = base_cp + i
        entry = glyphs.get(cp)
        if entry is None:
            sys.exit(f"ERROR: U+{cp:04X} missing from {BDF_PATH} "
                      f"(re-run with the full, non-subset Galmuri11.bdf "
                      f"if this codepoint was trimmed out by mistake)")
        canvas, ctop, cbot = place_glyph(entry)
        if ctop or cbot:
            clip_counter[0] += 1
        row = ",".join(format_row_c_literal(r) for r in canvas)
        f.write(f"{{{row}}},")
        if (i + 1) % 4 == 0:
            f.write("\n")
    f.write("\n};\n\n")


def main():
    glyphs = parse_bdf(BDF_PATH)
    clip_counter = [0]  # mutable int-in-a-list so emit_table() can bump it

    with open(OUT_PATH, "w") as f:
        f.write("#ifndef FONT_KO_DATA_H\n#define FONT_KO_DATA_H\n")
        f.write("#include \"io.h\"\n\n")
        f.write("/* Auto-generated 11x11 bitmap glyphs for all 11172 modern Hangul\n")
        f.write(" * syllables (U+AC00 to U+D7A3) plus the 51 standalone compatibility\n")
        f.write(" * jamo (U+3131 to U+3163, used to show an in-progress consonant or\n")
        f.write(" * vowel before it combines into a full syllable), extracted from\n")
        f.write(" * Galmuri11 (see third_party/galmuri-font/, SIL Open Font License\n")
        f.write(" * 1.1) by tools/gen_hangul_font.py. Do not hand-edit -- regenerate\n")
        f.write(" * with that script if the font ever changes.\n")
        f.write(" *\n")
        f.write(" * Each glyph is 11 rows of u16, MSB-aligned exactly like\n")
        f.write(" * font_latin_data.h's Latin glyphs -- see that header for the bit\n")
        f.write(" * layout. Index font8x8_ko with (codepoint - KO_SYLLABLE_BASE), and\n")
        f.write(" * font8x8_ko_jamo with (codepoint - KO_JAMO_BASE). (Table names kept\n")
        f.write(" * as font8x8_ko/font8x8_ko_jamo for now even though glyphs are 11x11\n")
        f.write(" * -- renaming them would just be churn across font_ko.h's call\n")
        f.write(" * sites for zero behavioral change.) */\n\n")
        f.write(f"#define KO_SYLLABLE_COUNT {COUNT}\n")
        f.write(f"#define KO_SYLLABLE_BASE  0x{BASE:04X}\n")
        f.write(f"#define KO_JAMO_COUNT {JAMO_COUNT}\n")
        f.write(f"#define KO_JAMO_BASE  0x{JAMO_BASE:04X}\n\n")

        emit_table(f, glyphs, BASE, COUNT, "font8x8_ko", clip_counter)
        emit_table(f, glyphs, JAMO_BASE, JAMO_COUNT, "font8x8_ko_jamo", clip_counter)

        f.write("#endif\n")

    size = os.path.getsize(OUT_PATH)
    print(f"Wrote {OUT_PATH} ({size} bytes, {COUNT} syllables + {JAMO_COUNT} jamo, "
          f"{clip_counter[0]} glyphs clipped)")


if __name__ == "__main__":
    main()
