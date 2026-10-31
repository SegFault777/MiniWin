#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Screen tour of SETTING.MWP -- the Control Panel (1.1-alpha-13) -- compared pixel for pixel with tools/test/golden_setting/*.png.
#   tools/test/ui_golden_setting.sh check     # compare (exit 1 on any difference)
#   tools/test/ui_golden_setting.sh record    # (re)write the goldens -- only when a visual change is INTENDED
# The icon grid, a page opened by double-click (Date/Time with a 24-hour clock, Display with a Teal desktop, Mouse, Text with kerning
# off), the keyboard (arrows + Enter), and the window shrunk until the grid needs its scroll bar. About ("System") is left out on purpose:
# it shows the version and the machine. NOT the pointer speed: the harness moves the mouse in relative steps, so a faster pointer would
# send every later click to the wrong place. The clicks are coordinates: the window opens at (100,40), icons are 100x74 cells.
set -uo pipefail
cd "$(dirname "$0")/../.."
MODE=${1:-check}; GOLDEN=${GOLDEN:-tools/test/golden_setting}; OUT=/tmp/ui_shots_setting
rm -rf $OUT; mkdir -p $OUT /tmp/www; cp tools/test/pages/ui_page.html /tmp/www/
LINES_OUT=1 tools/test/qemu_shot.sh ui_page.html \
  "wait 22" "move 50 84" "click" "click" "wait 2" "shot $OUT/s30_grid.ppm" \
  "move 155 92" "click" "click" "wait 1" "move 130 194" "click" "wait 1" "shot $OUT/s31_datetime.ppm" \
  "move 130 85" "click" "wait 1" "move 255 92" "click" "click" "wait 1" "move 140 142" "click" "wait 1" "shot $OUT/s32_display.ppm" \
  "move 130 85" "click" "wait 1" "move 155 166" "click" "click" "wait 1" "move 140 160" "click" "wait 1" "shot $OUT/s33_mouse.ppm" \
  "move 130 85" "click" "wait 1" "move 355 240" "click" "click" "wait 1" "move 140 142" "click" "wait 1" "shot $OUT/s34_kerning_off.ppm" \
  "key esc" "wait 1" "key left" "key up" "wait 1" "shot $OUT/s35_keyboard.ppm" \
  "drag 412 330 290 190" "wait 1" "shot $OUT/s36_small.ppm" \
  >/dev/null 2>&1
python3 tools/test/ui_diff.py "$MODE" $OUT "$GOLDEN"
