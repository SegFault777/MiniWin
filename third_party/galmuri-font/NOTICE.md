# Galmuri11 (갈무리11)

Source: https://github.com/quiple/galmuri
License: SIL Open Font License 1.1 (see LICENSE in this directory)
Copyright (c) 2019-2025 Lee Minseo (quiple@quiple.dev)

`Galmuri11-subset.bdf` is a trimmed copy of the upstream `dist/Galmuri11.bdf`
bitmap font, keeping only the glyphs MiniWin actually renders: ASCII
0x20-0x7E, the 11172 modern Hangul syllables (U+AC00-U+D7A3), and the 51
standalone compatibility jamo (U+3131-U+3163). Trimming is a mechanical
subset (same STARTCHAR blocks, byte-for-byte, just fewer of them) -- no
glyph was redrawn or modified. This keeps the vendored file a fraction of
upstream's ~2.9MB (which covers CJK ideographs and Latin Extended ranges
MiniWin has no use for) while remaining a faithful, regeneratable copy of
Galmuri's actual bitmap data.

Both the English (kernel/font.h) and Hangul (kernel/font_ko.h,
font_ko_data.h) glyph tables are generated from this BDF by
tools/gen_latin_font.py and tools/gen_hangul_font.py respectively, so the
whole UI shares one consistent 11x11 bitmap style instead of mixing two
unrelated typefaces. Regenerate both if this BDF is ever updated.
