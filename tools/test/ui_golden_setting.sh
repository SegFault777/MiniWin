#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Screen tour of SETTING.MWP (1.1-alpha-12), compared pixel for pixel with tools/test/golden_setting/*.png.
#   tools/test/ui_golden_setting.sh check     # compare (exit 1 on any difference)
#   tools/test/ui_golden_setting.sh record    # (re)write the goldens -- only when a visual change is INTENDED
# Covers the pages whose look does not depend on the version or the machine (About is left out on purpose), what a click on a line
# changes (24-hour clock, desktop colour, double-click time, kerning; NOT the pointer speed: the tour moves the mouse by relative
# steps, so a faster pointer would send every later click to the wrong place) and the window shrunk until its scroll bars appear.
set -uo pipefail
cd "$(dirname "$0")/../.."
MODE=${1:-check}; GOLDEN=${GOLDEN:-tools/test/golden_setting}; OUT=/tmp/ui_shots_setting
rm -rf $OUT; mkdir -p $OUT /tmp/www; cp tools/test/pages/ui_page.html /tmp/www/
LINES_OUT=1 tools/test/qemu_shot.sh ui_page.html \
  "wait 22" "move 50 84" "click" "click" "wait 2" "shot $OUT/s30_language.ppm" \
  "move 160 128" "click" "move 260 68" "click" "move 260 86" "click" "wait 1" "shot $OUT/s31_display.ppm" \
  "move 160 144" "click" "move 260 86" "click" "wait 1" "shot $OUT/s32_mouse.ppm" \
  "move 160 176" "click" "wait 1" "shot $OUT/s33_text.ppm" \
  "move 260 68" "click" "wait 1" "shot $OUT/s34_kerning_off.ppm" \
  "drag 520 240 330 130" "wait 1" "shot $OUT/s35_small.ppm" \
  >/dev/null 2>&1
python3 tools/test/ui_diff.py "$MODE" $OUT "$GOLDEN"
