#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""mwp_pack.py -- append the optional metadata trailer to a .mwp: the program's own icon (16x16 and 32x32), a title, and the
oldest ABI version it needs.   (1.1-alpha-11; layout documented in sdk/README.md and docs/dev/MWP-PROGRAMS.md)

    tools/mwp_pack.py in.mwp out.mwp [--icon icon.png] [--title "MiniCalc"] [--abi 3]

A .mwp is a raw flat binary with no header, so the metadata goes at the END, where an old kernel never looks (it loads the file
as it always did and the extra bytes are simply never executed) and a new one finds it from the last 32 bytes:

    [ program bytes ... ][ title\\0 ][ icon16: 16*16 RGBA ][ icon32: 32*32 RGBA ][ trailer: 32 bytes ]

    trailer (little-endian)   0  "MWPX"        4  u16 version (1)     6  u16 flags: 1 icon16, 2 icon32, 4 title
                              8  u32 payload_len = bytes of program proper (what the loader copies; the entry is at offset 0)
                             12  u32 icon16_off  16  u32 icon32_off  20  u32 title_off   (offsets from the file start, 0 = none)
                             24  u32 min_abi (the kernel refuses to start the program if its table is older)   28  u32 reserved

The icon PNG may be any size (8-bit grey, grey+alpha, RGB, RGBA or palette; not interlaced): it is scaled to both sizes with a box
filter (down) or nearest neighbour (up). Standard library only -- no Pillow needed.
"""
import struct, sys, zlib, argparse

def read_png(path):
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{path}: not a PNG file")
    pos, ihdr, idat, plte, trns = 8, None, b"", None, None
    while pos < len(d):
        ln, typ = struct.unpack(">I4s", d[pos:pos + 8]); body = d[pos + 8:pos + 8 + ln]; pos += 12 + ln
        if typ == b"IHDR": ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE": plte = body
        elif typ == b"tRNS": trns = body
        elif typ == b"IDAT": idat += body
        elif typ == b"IEND": break
    w, h, depth, ctype, _, _, interlace = ihdr
    if depth != 8 or interlace: sys.exit(f"{path}: only 8-bit, non-interlaced PNGs are supported (re-save it that way)")
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat); stride = w * ch; rows = []; prev = bytearray(stride); p = 0
    for _ in range(h):
        f = raw[p]; line = bytearray(raw[p + 1:p + 1 + stride]); p += 1 + stride
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0; b = prev[i]; c = prev[i - ch] if i >= ch else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(line); prev = line
    px = []
    for line in rows:
        for x in range(w):
            if ctype == 6: r, g, b, a = line[x * 4:x * 4 + 4]
            elif ctype == 2: r, g, b = line[x * 3:x * 3 + 3]; a = 255
            elif ctype == 0: r = g = b = line[x]; a = 255
            elif ctype == 4: r = g = b = line[x * 2]; a = line[x * 2 + 1]
            else:
                i = line[x]; r, g, b = plte[i * 3:i * 3 + 3]; a = trns[i] if trns and i < len(trns) else 255
            px.append((r, g, b, a))
    return w, h, px

def scale(w, h, px, n):
    """n x n RGBA bytes. Box filter when shrinking (alpha-weighted so edges do not fringe), nearest when growing."""
    out = bytearray()
    for y in range(n):
        y0, y1 = y * h // n, max(y * h // n + 1, (y + 1) * h // n)
        for x in range(n):
            x0, x1 = x * w // n, max(x * w // n + 1, (x + 1) * w // n)
            sa = sr = sg = sb = cnt = 0
            for yy in range(y0, y1):
                for xx in range(x0, x1):
                    r, g, b, a = px[yy * w + xx]; sa += a; sr += r * a; sg += g * a; sb += b * a; cnt += 1
            a = sa // cnt
            out += bytes((sr // sa, sg // sa, sb // sa, a)) if sa else bytes((0, 0, 0, 0))
    return bytes(out)

def pack(data, icon=None, title=None, abi=1):
    if data[-32:-28] == b"MWPX":                       # already packed: start again from the program proper
        data = data[:struct.unpack("<I", data[-24:-20])[0]]
    payload = len(data); body = bytearray(data); flags = 0; i16 = i32 = tit = 0
    if title:
        tb = title.encode("utf-8")[:23] + b"\0"; tit = len(body); body += tb; flags |= 4
    if icon:
        w, h, px = read_png(icon)
        while len(body) % 4: body += b"\0"
        i16 = len(body); body += scale(w, h, px, 16); i32 = len(body); body += scale(w, h, px, 32); flags |= 3
    if not flags and abi <= 1: return data
    body += b"MWPX" + struct.pack("<HHIIIIII", 1, flags, payload, i16, i32, tit, abi, 0)
    return bytes(body)

if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="append the metadata trailer to a .mwp")
    ap.add_argument("inp"); ap.add_argument("out"); ap.add_argument("--icon"); ap.add_argument("--title"); ap.add_argument("--abi", type=int, default=1)
    a = ap.parse_args()
    out = pack(open(a.inp, "rb").read(), a.icon, a.title, a.abi)
    open(a.out, "wb").write(out)
    print(f"  mwp_pack: {len(out)} bytes ({len(out) - len(open(a.inp, 'rb').read())} of metadata)")
