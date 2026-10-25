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
LINES_OUT=1 tools/test/qemu_shot.sh ui_page.html \
  "wait 22" "move 50 261" "click" "click" "wait 3" "shot $OUT/s20_files.ppm" \
  "move 180 149" "click" "wait 1" "shot $OUT/s21_selected.ppm" \
  "move 235 316" "click" "wait 1" "shot $OUT/s22_delete_armed.ppm" \
  "move 222 127" "click" "wait 2" "shot $OUT/s23_disks.ppm" \
  "move 150 316" "click" "wait 1" "shot $OUT/s24_no_selection.ppm" \
  "move 180 149" "click" "wait 1" "move 150 316" "click" "wait 1" "shot $OUT/s25_running_refused.ppm" \
  >/dev/null 2>&1
python3 tools/test/ui_diff.py "$MODE" $OUT "$GOLDEN"
