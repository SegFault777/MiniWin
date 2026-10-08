#!/usr/bin/env python3
"""Compares a directory of fresh screenshots (PPM) against the golden PNGs, pixel for pixel.
   ui_diff.py check  NEWDIR GOLDENDIR      -> exit 1 if anything differs (prints where)
   ui_diff.py record NEWDIR GOLDENDIR      -> overwrite the goldens with the new shots
The taskbar clock changes every minute, so its rectangle is ignored; everything else must match exactly."""
import os, sys
from PIL import Image
MASK = (540, 460, 640, 480)          # the clock readout + a little slack (x0, y0, x1, y1)
mode, new, gold = sys.argv[1:4]
names = sorted(f[:-4] for f in os.listdir(new) if f.endswith(".ppm"))
bad = 0
os.makedirs(gold, exist_ok=True)
for n in names:
    im = Image.open(os.path.join(new, n + ".ppm")).convert("RGB")
    gp = os.path.join(gold, n + ".png")
    if mode == "record":
        im.save(gp, optimize=True); print("recorded", n); continue
    if not os.path.exists(gp): print("MISSING golden", n); bad += 1; continue
    g = Image.open(gp).convert("RGB")
    if g.size != im.size: print("SIZE differs", n); bad += 1; continue
    a, b = im.load(), g.load(); diff = 0; box = [9999, 9999, -1, -1]
    for y in range(im.height):
        for x in range(im.width):
            if MASK[0] <= x < MASK[2] and MASK[1] <= y < MASK[3]: continue
            if a[x, y] != b[x, y]:
                diff += 1; box = [min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)]
    if diff: print("DIFF   %s: %d pixels differ, bbox %s" % (n, diff, box)); bad += 1; im.save("/tmp/ui_fail_%s.png" % n)
    else: print("same   %s" % n)
sys.exit(1 if bad else 0)
