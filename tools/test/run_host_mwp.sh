#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# The .mwp metadata trailer (kernel/mwp_trailer.h, files made by tools/mwp_pack.py) and the SDK's small C library (sdk/include/mw_libc.h)
# on the host, with the address and undefined-behaviour sanitizers on.
set -euo pipefail
cd "$(dirname "$0")/../.."
D=/tmp/mwp_test_files; rm -rf "$D"; mkdir -p "$D"
python3 - "$D" <<'PY'
import sys, struct, os, random
sys.path.insert(0, "tools")
import mwp_pack
d = sys.argv[1]
# a 40x40 test picture, two colours with a transparent border
w = h = 40
px = [(220, 80, 30, 255) if (4 <= x < 36 and 4 <= y < 36 and (x + y) % 5) else ((0, 0, 0, 0) if x < 4 else (30, 90, 200, 255)) for y in range(h) for x in range(w)]
import zlib
def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
raw = b"".join(b"\0" + bytes(c for p in px[y * w:(y + 1) * w] for c in p) for y in range(h))
open(d + "/icon.png", "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
prog = bytes(random.Random(5).randrange(256) for _ in range(200))
open(d + "/plain.mwp", "wb").write(prog)
open(d + "/full.mwp", "wb").write(mwp_pack.pack(prog, d + "/icon.png", "MiniCalc", 3))
open(d + "/title_only.mwp", "wb").write(mwp_pack.pack(prog, None, "OnlyTitle", 1))
# packing an already packed file starts again from the program proper (same bytes, not a trailer on a trailer)
assert mwp_pack.pack(open(d + "/full.mwp", "rb").read(), d + "/icon.png", "MiniCalc", 3) == open(d + "/full.mwp", "rb").read()
PY
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -fsanitize=undefined,address -o /tmp/host_mwp_test tools/test/host_mwp_test.c
/tmp/host_mwp_test "$D"
