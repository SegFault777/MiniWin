#!/usr/bin/env bash
# Boots the OS in QEMU, drives the desktop through a fixed tour (open every app, type, open menus, press a
# button without releasing) and compares every screenshot with tools/test/golden/*.png, pixel for pixel.
#   tools/test/ui_golden.sh check [theme]    # compare (exit 1 on any difference); theme defaults to classic
#   tools/test/ui_golden.sh record [theme]   # (re)write the goldens -- only when a visual change is INTENDED
# Goldens live in tools/test/golden (classic) and tools/test/golden_<theme>. Env: GOLDEN overrides the dir.
set -uo pipefail
cd "$(dirname "$0")/../.."
MODE=${1:-check}; TH=${2:-classic}
if [ "$TH" = classic ]; then GOLDEN=${GOLDEN:-tools/test/golden}; else GOLDEN=${GOLDEN:-tools/test/golden_$TH}; fi
export THEME=$TH; OUT=/tmp/ui_shots
rm -rf $OUT; mkdir -p $OUT /tmp/www; cp tools/test/pages/ui_page.html /tmp/www/
LINES_OUT=1 tools/test/qemu_shot.sh ui_page.html \
  "wait 22" "shot $OUT/s00_desktop.ppm" \
  "move 343 25" "click" "click" "wait 2" "move 300 307" "click" "type apps" "key ret" "wait 1" "shot $OUT/s01_terminal.ppm" \
  "move 245 25" "click" "click" "wait 3" "shot $OUT/s02_web.ppm" \
  "move 148 25" "click" "click" "wait 2" "shot $OUT/s03_setting.ppm" \
  "move 50 25" "click" "click" "wait 2" "type Hello MiniWin" "wait 1" "shot $OUT/s04_notepad.ppm" \
  "move 40 38" "click" "wait 1" "shot $OUT/s05_filemenu.ppm" \
  "move 400 400" "click" "move 18 470" "click" "wait 1" "shot $OUT/s06_startmenu.ppm" \
  "move 450 400" "click" "move 314 20" "raw mouse_button 1" "wait 1" "shot $OUT/s07_pressed.ppm" "move 200 300" "raw mouse_button 0" "wait 1" "shot $OUT/s08_cancelled.ppm" \
  "drag 120 20 260 130" "wait 1" "shot $OUT/s09_dragged.ppm" \
  "drag 484 292 540 340" "wait 1" "shot $OUT/s10_resized.ppm" \
  "move 517 131" "click" "wait 1" "shot $OUT/s11_maximized.ppm" \
  "move 601 7" "click" "wait 1" "shot $OUT/s12_minimized.ppm" \
  "move 116 470" "click" "wait 1" "shot $OUT/s13_restored.ppm" \
  "move 632 7" "click" "wait 1" "shot $OUT/s14_confirm.ppm" \
  >/dev/null 2>&1
python3 tools/test/ui_diff.py "$MODE" $OUT "$GOLDEN"
