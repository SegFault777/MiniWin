#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# The compressed Korean font: the checked-in header is up to date and round-trips, and the kernel's LZ4 decoder
# (kernel/lz.h) inflates it to exactly the bytes the generator says, and survives hostile input (ASan/UBSan).
#   tools/test/run_host_font.sh
set -euo pipefail
cd "$(dirname "$0")"
python3 ../gen_hangul_font.py --check
python3 ../gen_cursors.py --check              # the cursor shapes are generated from the icon pack too
python3 ../gen_hangul_font.py --dump-raw /tmp/ko_raw.bin
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-int-to-pointer-cast -fsanitize=address,undefined -fno-sanitize-recover=undefined -o /tmp/host_lz_test host_lz_test.c
/tmp/host_lz_test /tmp/ko_raw.bin
