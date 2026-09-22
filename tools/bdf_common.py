"""Shared BDF-parsing and 11x11-cell glyph placement logic used by both
gen_latin_font.py and gen_hangul_font.py, so the two generators agree on
exactly how a BDF glyph lands in MiniWin's fixed-size bitmap cell instead
of each reinventing (and potentially disagreeing on) the same math.

Not a general-purpose BDF library -- just enough of the format to read
Galmuri11-subset.bdf's STARTCHAR blocks. If you ever point this at a BDF
that uses features Galmuri doesn't (multi-plane encodings, non-integer
metrics, vertical writing), it'll probably choke, and that's fine.
"""
import re

CELL = 11  # every glyph MiniWin draws lives in one 11x11 bit cell, full
           # stop -- see the module docstring in gen_latin_font.py for why
           # this single fixed size, rather than per-glyph advance widths,
           # was the right call for a freestanding kernel with no font
           # shaping engine.
BASELINE_ROW = 10  # 0-indexed row (of 0..CELL-1) that the font's baseline
                    # sits on. Chosen so full-height glyphs (capital
                    # letters, digits, every Hangul syllable) land flush
                    # with zero clipping -- see gen_latin_font.py's
                    # docstring for the descender trade-off this implies.


def parse_bdf(path):
    """Returns {codepoint: {'bw','bh','bxoff','byoff','dwidth','rows'}}.
    rows is the raw list of BDF hex strings, one per pixel row, exactly as
    written in the BITMAP section -- still needs decode_rows() below to
    become actual bits."""
    with open(path, encoding="utf-8", errors="replace") as f:
        content = f.read()
    blocks = content.split("STARTCHAR ")[1:]
    out = {}
    for b in blocks:
        rest = b.split("\n", 1)[1]
        enc_m = re.search(r"ENCODING (-?\d+)", rest)
        bbx_m = re.search(r"BBX (-?\d+) (-?\d+) (-?\d+) (-?\d+)", rest)
        if not enc_m or not bbx_m:
            continue
        enc = int(enc_m.group(1))
        bw, bh, bxoff, byoff = (int(x) for x in bbx_m.groups())
        dwidth_m = re.search(r"DWIDTH (-?\d+) (-?\d+)", rest)
        dwidth = int(dwidth_m.group(1)) if dwidth_m else bw
        rows = []
        bmp = rest.split("BITMAP\n", 1)
        if len(bmp) == 2:
            for line in bmp[1].split("ENDCHAR")[0].strip("\n").split("\n"):
                line = line.strip()
                if line:
                    rows.append(line)
        out[enc] = {"bw": bw, "bh": bh, "bxoff": bxoff, "byoff": byoff,
                     "dwidth": dwidth, "rows": rows}
    return out


def decode_rows(entry):
    """BDF pads each bitmap row out to a whole number of hex digits (4
    bits each), left-aligned -- so a 7-bit-wide glyph's row is stored as
    2 hex chars (8 bits) with the low bit unused, not 7 bits exactly.
    This strips that padding and returns bh ints, each bw bits wide,
    MSB = glyph's leftmost pixel."""
    bits = []
    for line in entry["rows"]:
        val = int(line, 16) if line else 0
        total_bits = len(line) * 4
        pad = total_bits - entry["bw"]
        if pad > 0:
            val >>= pad
        bits.append(val)
    return bits


def place_glyph(entry, cell=CELL, baseline_row=BASELINE_ROW):
    """Places one BDF glyph into an 11-row canvas, baseline-aligned.
    Returns (canvas, clipped_top, clipped_bottom) -- canvas is a list of
    `cell` ints, each `cell` bits wide (bit (cell-1) = leftmost pixel),
    ready to write out as a fixed-width bitmap row. clipped_* count any
    source rows that fell outside the cell (see gen_latin_font.py: this
    only ever happens to the last 1-2px of a handful of descenders --
    g/j/p/q/y/comma -- and is an accepted, deliberate trade-off, not a
    bug to chase)."""
    bw, bh = entry["bw"], entry["bh"]
    bxoff, byoff = entry["bxoff"], entry["byoff"]
    bits = decode_rows(entry)
    canvas = [0] * cell
    clipped_top = clipped_bottom = 0
    # Row 0 of `bits` is the glyph's topmost pixel row, which sits
    # (byoff + bh) pixels above the baseline. baseline_row is itself the
    # canvas row the baseline occupies, so the topmost row lands at
    # baseline_row - (byoff + bh) + 1.
    top_canvas_row = baseline_row - (byoff + bh) + 1
    for i, rowval in enumerate(bits):
        cr = top_canvas_row + i
        if cr < 0:
            clipped_top += 1
            continue
        if cr >= cell:
            clipped_bottom += 1
            continue
        shift = cell - bw - bxoff
        canvas[cr] |= (rowval << shift) if shift >= 0 else (rowval >> -shift)
    return canvas, clipped_top, clipped_bottom


def format_row_c_literal(row_bits, cell=CELL):
    """Formats one placed glyph row as a C array initializer element.
    cell=11 doesn't fit a u8, so every row is stored as a u16 with the
    top `cell` bits holding the pixel data (bit 15 = leftmost pixel) --
    see kernel/font.h's font_draw_char() for the matching reader."""
    val = row_bits << (16 - cell)
    return f"0x{val:04X}"
