#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Builds a program and writes it into /progs of a disk image, so it shows up on the desktop and in MiniFiles at next boot:
#   sdk/install.sh sdk/examples/hello.c HELLO.MWP [build/os-image.img]
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -ge 2 ] || { echo "usage: sdk/install.sh <program.c> <NAME.MWP> [image]"; exit 1; }
SRC="$1"; NAME="$2"; IMG="${3:-build/os-image.img}"
tools/build_mwp.sh "$SRC" >/dev/null
python3 tools/install_mwp.py "$IMG" 0 "$NAME" "build/$(basename "$SRC" .c).mwp"
echo "installed $NAME into $IMG"
