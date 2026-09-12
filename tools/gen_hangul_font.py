#!/usr/bin/env python3
"""Generates kernel/font_ko_data.h: an 8x8 bitmap glyph table for all 11172
modern Hangul syllables (U+AC00-U+D7A3), extracted from the bundled
Dalmoori (달무리) font (third_party/dalmoori-font/dalmoori.ttf,
Apache License 2.0). Re-run this script if the font ever changes;
the generated header is otherwise treated as a build artifact checked
into source for convenience (no Node/font-build toolchain needed to
build the kernel).
"""
import os
from PIL import Image, ImageFont, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_PATH = os.path.join(HERE, "..", "third_party", "dalmoori-font", "dalmoori.ttf")
OUT_PATH = os.path.join(HERE, "..", "kernel", "font_ko_data.h")

BASE = 0xAC00
COUNT = 11172  # 0xD7A3 - 0xAC00 + 1

JAMO_BASE = 0x3131
JAMO_COUNT = 51  # U+3131-U+3163: standalone consonants+vowels, used to
                 # show an in-progress jamo before it combines into a full
                 # syllable (e.g. showing just "ㄱ" while composing).

def render_bitmap(font, ch):
    img = Image.new("L", (8, 8), 0)
    draw = ImageDraw.Draw(img)
    draw.text((0, 0), ch, font=font, fill=255)
    rows = []
    for y in range(8):
        byte = 0
        for x in range(8):
            if img.getpixel((x, y)) > 128:
                byte |= (0x80 >> x)
        rows.append(byte)
    return rows

def main():
    font = ImageFont.truetype(FONT_PATH, 8)

    with open(OUT_PATH, "w") as f:
        f.write("#ifndef FONT_KO_DATA_H\n")
        f.write("#define FONT_KO_DATA_H\n")
        f.write("#include \"io.h\"\n\n")
        f.write("/* Auto-generated 8x8 bitmap glyphs for all 11172 modern Hangul\n")
        f.write(" * syllables (U+AC00 to U+D7A3) plus the 51 standalone compatibility\n")
        f.write(" * jamo (U+3131 to U+3163, used to show an in-progress consonant or\n")
        f.write(" * vowel before it combines into a full syllable), extracted from\n")
        f.write(" * the Dalmoori (달무리) font (see third_party/dalmoori-font/,\n")
        f.write(" * Apache License 2.0) by tools/gen_hangul_font.py. Do not hand-edit\n")
        f.write(" * -- regenerate with that script if the font ever changes.\n")
        f.write(" *\n")
        f.write(" * Index font8x8_ko with (codepoint - KO_SYLLABLE_BASE), and\n")
        f.write(" * font8x8_ko_jamo with (codepoint - KO_JAMO_BASE). */\n\n")
        f.write(f"#define KO_SYLLABLE_COUNT {COUNT}\n")
        f.write(f"#define KO_SYLLABLE_BASE  0x{BASE:04X}\n")
        f.write(f"#define KO_JAMO_COUNT {JAMO_COUNT}\n")
        f.write(f"#define KO_JAMO_BASE  0x{JAMO_BASE:04X}\n\n")

        f.write("static const u8 font8x8_ko[KO_SYLLABLE_COUNT][8] = {\n")
        for i in range(COUNT):
            cp = BASE + i
            ch = chr(cp)
            bmp = render_bitmap(font, ch)
            row = ",".join(f"0x{b:02X}" for b in bmp)
            f.write(f"{{{row}}},")
            if (i + 1) % 8 == 0:
                f.write("\n")
        f.write("\n};\n\n")

        f.write("static const u8 font8x8_ko_jamo[KO_JAMO_COUNT][8] = {\n")
        for i in range(JAMO_COUNT):
            cp = JAMO_BASE + i
            ch = chr(cp)
            bmp = render_bitmap(font, ch)
            row = ",".join(f"0x{b:02X}" for b in bmp)
            f.write(f"{{{row}}},")
            if (i + 1) % 8 == 0:
                f.write("\n")
        f.write("\n};\n\n#endif\n")

    size = os.path.getsize(OUT_PATH)
    print(f"Wrote {OUT_PATH} ({size} bytes, {COUNT} syllables + {JAMO_COUNT} jamo)")

if __name__ == "__main__":
    main()
