#!/usr/bin/env python3
"""Generates kernel/font_latin_data.h: an 11x11 bitmap glyph table for
ASCII 0x20-0x7E, extracted from Galmuri11 (third_party/galmuri-font/,
SIL OFL 1.1) -- the same font kernel/font_ko_data.h's Hangul glyphs come
from, so the whole UI shares one consistent bitmap typeface instead of
mixing two unrelated designs the way the old hand-drawn 8x8 + Dalmoori
combination did.

Why 11x11 and not, say, 11 tall by however-wide-each-letter-actually-is:
this kernel's renderer (font_draw_char() in font.h) is a dumb fixed-cell
blitter with no font-shaping engine behind it -- every character takes
the same advance width no matter how it's drawn. Variable-width glyphs
would need per-character advance metrics threaded through every caller
that currently just does `x += CHAR_W` (window titles, status text,
Notepad's line wrapping, MiniWeb's response viewer...), which is a much
bigger change than "make the font bigger." Fixed-width was already this
codebase's design at 8x8; 11x11 keeps that same simplicity at the new
size.

Why row 10 is the baseline (see tools/bdf_common.py's BASELINE_ROW): an
11-pixel cell can't fit Galmuri11's full 11px cap-height AND its full
2px descender without clipping something, and since the same font's
Hangul syllables (which have zero descent -- see gen_hangul_font.py)
need to share a visual baseline with the Latin glyphs for mixed-language
text to line up, cap-height won by keeping it uncompromised: capital
letters, digits, and punctuation all render pixel-perfect, while
g/j/p/q/y's descender tail and the comma's tail lose their very bottom
1-2px. Given this kernel's UI is almost entirely uppercase labels, status
text, and now a URL bar, that's the right side to compromise on.

Re-run this script if third_party/galmuri-font/Galmuri11-subset.bdf ever
changes (see gen_hangul_font.py for the matching Hangul generator, and
NOTICE.md in that directory for where the subset came from). The
generated header is a checked-in build artifact, same as
font_ko_data.h -- no BDF-parsing toolchain needed just to build the
kernel.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bdf_common import parse_bdf, place_glyph, format_row_c_literal, CELL

HERE = os.path.dirname(os.path.abspath(__file__))
BDF_PATH = os.path.join(HERE, "..", "third_party", "galmuri-font", "Galmuri11-subset.bdf")
OUT_PATH = os.path.join(HERE, "..", "kernel", "font_latin_data.h")

FIRST_CP = 0x20  # space
LAST_CP = 0x7E   # tilde -- covers space through the full printable ASCII range


def main():
    glyphs = parse_bdf(BDF_PATH)
    count = LAST_CP - FIRST_CP + 1
    clipped_glyphs = 0

    with open(OUT_PATH, "w") as f:
        f.write("#ifndef FONT_LATIN_DATA_H\n#define FONT_LATIN_DATA_H\n")
        f.write("#include \"io.h\"\n\n")
        f.write("/* Auto-generated 11x11 bitmap glyphs for printable ASCII\n")
        f.write(" * (U+0020 space through U+007E tilde), extracted from Galmuri11\n")
        f.write(" * (see third_party/galmuri-font/, SIL Open Font License 1.1) by\n")
        f.write(" * tools/gen_latin_font.py. Do not hand-edit -- regenerate with that\n")
        f.write(" * script if the font ever changes.\n")
        f.write(" *\n")
        f.write(" * Each glyph is 11 rows of u16, MSB-aligned: bit 15 is the\n")
        f.write(" * leftmost pixel, bits 15..5 hold the 11 real pixel columns, bits\n")
        f.write(" * 4..0 are always zero (dead space, not a 5th unused pixel column\n")
        f.write(" * -- there simply aren't more than 11 columns of glyph data).\n")
        f.write(" * Index with (codepoint - FONT_LATIN_FIRST_CP).\n")
        f.write(" * A handful of descenders (g,j,p,q,y, comma) lose their very\n")
        f.write(" * bottom 1-2px to fit the fixed 11-row cell -- see this script's\n")
        f.write(" * own docstring for why that's the deliberate trade-off, not a\n")
        f.write(" * bug. */\n\n")
        f.write(f"#define FONT_LATIN_FIRST_CP 0x{FIRST_CP:02X}\n")
        f.write(f"#define FONT_LATIN_COUNT {count}\n\n")
        f.write(f"static const u16 font_latin[FONT_LATIN_COUNT][{CELL}] = {{\n")

        for cp in range(FIRST_CP, LAST_CP + 1):
            entry = glyphs.get(cp)
            if entry is None:
                # Shouldn't happen -- Galmuri11 covers all of printable
                # ASCII -- but fail loudly rather than emit a silently
                # blank glyph if the BDF is ever swapped for one that
                # doesn't.
                sys.exit(f"ERROR: U+{cp:04X} ({chr(cp)!r}) missing from {BDF_PATH}")
            canvas, ctop, cbot = place_glyph(entry)
            if ctop or cbot:
                clipped_glyphs += 1
            row = ",".join(format_row_c_literal(r) for r in canvas)
            f.write(f"{{{row}}}, /* U+{cp:04X} {chr(cp)!r} */\n")

        f.write("};\n\n#endif\n")

    size = os.path.getsize(OUT_PATH)
    print(f"Wrote {OUT_PATH} ({size} bytes, {count} glyphs, "
          f"{clipped_glyphs} with a clipped descender)")


if __name__ == "__main__":
    main()
