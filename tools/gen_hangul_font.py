#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
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

Since 1.1-alpha-2 the 11172-syllable table is stored COMPRESSED (standard LZ4
block format, see kernel/lz.h) and inflated into high memory at boot
(memmap.h: MW_KFONT_ADDR): the raw table was 245,784 bytes, nearly half of the
kernel image, and the image has a hard 512 KB budget. The inflated bytes are
exactly what the old `static const u16 font8x8_ko[11172][11]` held, so no
glyph changed. `--check` regenerates in memory and verifies both that the
checked-in header is up to date and that it round-trips.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bdf_common import parse_bdf, place_glyph, CELL

HERE = os.path.dirname(os.path.abspath(__file__))
BDF_PATH = os.path.join(HERE, "..", "third_party", "galmuri-font", "Galmuri11-subset.bdf")
OUT_PATH = os.path.join(HERE, "..", "kernel", "font_ko_data.h")

BASE = 0xAC00
COUNT = 11172  # 0xD7A3 - 0xAC00 + 1

JAMO_BASE = 0x3131
JAMO_COUNT = 51  # U+3131-U+3163: standalone consonants+vowels, used to
                 # show an in-progress jamo before it combines into a full
                 # syllable (e.g. showing just "ㄱ" while composing).


def collect(glyphs, base_cp, count, clip_counter):
    """The glyph rows of one table, flattened to a list of u16 (CELL rows per glyph)."""
    rows = []
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
        rows.extend(r << (16 - CELL) for r in canvas)   # MSB-aligned u16 rows, as the kernel reads them
    return rows


def lz4_compress(data, chain=512):
    """A greedy LZ4 *block* compressor (hash chains, minimum match 4). Not the fastest, not the tightest -- it only
    runs when the font changes -- but its output is plain LZ4 block format, which kernel/lz.h decodes."""
    n = len(data)
    heads = {}
    prev = [-1] * n
    out = bytearray()
    anchor = 0
    i = 0
    last_match_start = n - 12          # the format wants the final 5 bytes (and a 12-byte tail) left as literals
    def emit(lit_start, lit_end, offset, mlen):
        lit = lit_end - lit_start
        tok = (min(lit, 15) << 4) | (min(mlen - 4, 15) if mlen else 0)
        out.append(tok)
        if lit >= 15:
            r = lit - 15
            while r >= 255:
                out.append(255); r -= 255
            out.append(r)
        out.extend(data[lit_start:lit_end])
        if mlen:
            out.extend(struct.pack("<H", offset))
            if mlen - 4 >= 15:
                r = mlen - 4 - 15
                while r >= 255:
                    out.append(255); r -= 255
                out.append(r)
    def insert(pos):
        if pos + 4 <= n:
            key = data[pos:pos + 4]
            prev[pos] = heads.get(key, -1)
            heads[key] = pos
    while i < n:
        best_len = 0
        best_off = 0
        if i <= last_match_start:
            key = data[i:i + 4]
            cand = heads.get(key, -1)
            tries = chain
            max_len = n - 5 - i
            while cand >= 0 and tries and i - cand <= 65535:
                length = 0
                while length < max_len and data[cand + length] == data[i + length]:
                    length += 1
                if length > best_len:
                    best_len, best_off = length, i - cand
                cand = prev[cand]
                tries -= 1
        if best_len >= 4:
            emit(anchor, i, best_off, best_len)
            for k in range(i, i + best_len):
                insert(k)
            i += best_len
            anchor = i
        else:
            insert(i)
            i += 1
    emit(anchor, n, 0, 0)
    return bytes(out)


def lz4_decompress(blob, size):
    """Reference decoder (mirrors kernel/lz.h) used by --check."""
    out = bytearray()
    i = 0
    while i < len(blob):
        tok = blob[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = blob[i]; i += 1; lit += b
                if b != 255: break
        out.extend(blob[i:i + lit]); i += lit
        if i >= len(blob): break
        off = blob[i] | (blob[i + 1] << 8); i += 2
        mlen = (tok & 15) + 4
        if (tok & 15) == 15:
            while True:
                b = blob[i]; i += 1; mlen += b
                if b != 255: break
        for _ in range(mlen):
            out.append(out[-off])
    assert len(out) == size, (len(out), size)
    return bytes(out)


def checksum(data):
    """32-bit FNV-1a over the inflated bytes; ko_font_init() recomputes it. (An earlier rotate-xor sum was linear over
    GF(2), so a damaged match offset that shifts a repetitive bitmap by a multiple of 32 bytes could cancel out and
    still pass -- the host test found exactly that. The multiply makes such cancellation implausible.)"""
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def build():
    glyphs = parse_bdf(BDF_PATH)
    clip = [0]
    ko = collect(glyphs, BASE, COUNT, clip)
    jamo = collect(glyphs, JAMO_BASE, JAMO_COUNT, clip)
    raw = b"".join(struct.pack("<H", v) for v in ko)
    packed = lz4_compress(raw)
    assert lz4_decompress(packed, len(raw)) == raw
    return raw, packed, jamo, clip[0]


def render_header(raw, packed, jamo):
    f = []
    w = f.append
    w("/* SPDX-License-Identifier: GPL-3.0-only */\n")
    w("#ifndef FONT_KO_DATA_H\n#define FONT_KO_DATA_H\n")
    w("#include \"memmap.h\"\n#include \"lz.h\"\n\n")
    w("/* Auto-generated 11x11 bitmap glyphs for all 11172 modern Hangul\n")
    w(" * syllables (U+AC00 to U+D7A3) plus the 51 standalone compatibility\n")
    w(" * jamo (U+3131 to U+3163, used to show an in-progress consonant or\n")
    w(" * vowel before it combines into a full syllable), extracted from\n")
    w(" * Galmuri11 (see third_party/galmuri-font/, SIL Open Font License\n")
    w(" * 1.1) by tools/gen_hangul_font.py. Do not hand-edit -- regenerate\n")
    w(" * with that script if the font ever changes.\n")
    w(" *\n")
    w(" * Each glyph is 11 rows of u16, MSB-aligned exactly like\n")
    w(" * font_latin_data.h's Latin glyphs -- see that header for the bit\n")
    w(" * layout. Index font8x8_ko with (codepoint - KO_SYLLABLE_BASE), and\n")
    w(" * font8x8_ko_jamo with (codepoint - KO_JAMO_BASE). (Table names kept\n")
    w(" * as font8x8_ko/font8x8_ko_jamo for now even though glyphs are 11x11\n")
    w(" * -- renaming them would just be churn across font_ko.h's call\n")
    w(" * sites for zero behavioral change.)\n")
    w(" *\n")
    w(f" * The 11172-syllable table is {len(raw)} bytes raw; it ships here LZ4-compressed ({len(packed)} bytes)\n")
    w(" * and ko_font_init() (below) inflates it to MW_KFONT_ADDR (memmap.h) before anything draws, so\n")
    w(" * font8x8_ko below is a pointer into high memory, not a table in the image. The 51 jamo stay as they are. */\n\n")
    w("#define KO_SYLLABLE_COUNT 11172\n#define KO_SYLLABLE_BASE  0xAC00\n")
    w("#define KO_JAMO_COUNT 51\n#define KO_JAMO_BASE  0x3131\n\n")
    w(f"#define KO_FONT_RAW_BYTES    {len(raw)}u\n")
    w(f"#define KO_FONT_PACKED_BYTES {len(packed)}u\n")
    w(f"#define KO_FONT_CHECKSUM     0x{checksum(raw):08X}u   /* FNV-1a over the inflated bytes */\n\n")
    w("#define font8x8_ko ((const u16 (*)[11])MW_KFONT_ADDR)\n\n")
    w("static const u8 font_ko_packed[KO_FONT_PACKED_BYTES] = {\n")
    for i in range(0, len(packed), 24):
        w(",".join(str(b) for b in packed[i:i + 24]) + ",\n")
    w("};\n\n")
    w("/* Inflates the syllable table to MW_KFONT_ADDR and verifies it. Returns 1 if every byte is where it belongs, 0 if\n")
    w(" * the table failed to decode or its checksum is wrong (a build bug; the caller reports it). Must run before\n")
    w(" * anything draws a Hangul glyph -- kernel_boot() does it right after the serial port is up. */\n")
    w("static int ko_font_init(void) {\n")
    w("    u8 *dst = (u8 *)(MW_KFONT_ADDR);\n")
    w("    if (lz4_block_decode(font_ko_packed, KO_FONT_PACKED_BYTES, dst, KO_FONT_RAW_BYTES) != KO_FONT_RAW_BYTES) return 0;\n")
    w("    u32 h = 0x811C9DC5u;\n")
    w("    for (u32 i = 0; i < KO_FONT_RAW_BYTES; i++) h = (h ^ dst[i]) * 0x01000193u;\n")
    w("    return h == KO_FONT_CHECKSUM;\n")
    w("}\n\n")
    w(f"static const u16 font8x8_ko_jamo[{JAMO_COUNT}][{CELL}] = {{\n")
    for i in range(JAMO_COUNT):
        w("{" + ",".join(f"0x{r:04X}" for r in jamo[i * CELL:(i + 1) * CELL]) + "},")
        if (i + 1) % 4 == 0:
            w("\n")
    w("\n};\n\n#endif\n")
    return "".join(f)


def main():
    raw, packed, jamo, clipped = build()
    text = render_header(raw, packed, jamo)
    if "--check" in sys.argv:
        with open(OUT_PATH) as fh:
            cur = fh.read()
        if cur != text:
            sys.exit("font_ko_data.h is out of date -- run tools/gen_hangul_font.py")
        print(f"font_ko_data.h up to date; {len(raw)} -> {len(packed)} bytes, round trip OK, checksum 0x{checksum(raw):08X}")
        return
    if "--dump-raw" in sys.argv:                       # for the host test: the exact bytes ko_font_init() must produce
        with open(sys.argv[sys.argv.index("--dump-raw") + 1], "wb") as fh:
            fh.write(raw)
        return
    with open(OUT_PATH, "w") as f:
        f.write(text)
    size = os.path.getsize(OUT_PATH)
    print(f"Wrote {OUT_PATH} ({size} bytes: {len(raw)} raw -> {len(packed)} LZ4, {COUNT} syllables + {JAMO_COUNT} jamo, "
          f"{clipped} glyphs clipped)")


if __name__ == "__main__":
    main()
