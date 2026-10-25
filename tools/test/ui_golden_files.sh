#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Screen tour of MINIFILES.MWP (1.1-alpha-1), compared pixel for pixel with tools/test/golden_files/*.png.
#   tools/test/ui_golden_files.sh check     # compare (exit 1 on any difference)
#   tools/test/ui_golden_files.sh record    # (re)write the goldens -- only when a visual change is INTENDED
# Separate from ui_golden.sh because that tour's later coordinates depend on exactly which windows it opened.
set -uo pipefail
cd "$(dirname "$0")/../.."
MODE=${1:-check}; GOLDEN=${GOLDEN:-tools/test/golden_files}; OUT=/tmp/ui_shots_files
rm -rf $OUT; mkdir -p $OUT /tmp/www; cp tools/test/pages/ui_page.html /tmp/www/
# The explorer tour. The RTC is pinned by qemu_shot.sh (-rtc base=2026-01-02...), so a new folder always shows the same date.
LINES_OUT=1 tools/test/qemu_shot.sh ui_page.html \
  "wait 22" "move 50 261" "click" "click" "wait 3" "shot $OUT/s20_explorer.ppm" \
  "move 90 164" "click" "wait 1" "shot $OUT/s21_progs.ppm" \
  "move 262 148" "click" "wait 1" "shot $OUT/s22_selected.ppm" \
  "move 297 78" "click" "wait 1" "shot $OUT/s23_delete_armed.ppm" \
  "move 92 78" "click" "wait 2" "shot $OUT/s24_new_folder.ppm" \
  "key ret" "wait 1" "move 330 78" "click" "wait 1" "shot $OUT/s25_renamed.ppm" \
  "move 180 59" "click" "wait 1" "shot $OUT/s26_view_menu.ppm" \
  >/dev/null 2>&1
python3 tools/test/ui_diff.py "$MODE" $OUT "$GOLDEN"
