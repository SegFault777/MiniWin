#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Builds a MiniWin program from one C file:   sdk/build.sh sdk/examples/hello.c      ->  build/hello.mwp
# (a thin front for tools/build_mwp.sh, which is what the OS's own build uses; run it from the repository root)
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -eq 1 ] || { echo "usage: sdk/build.sh <program.c>   (e.g. sdk/examples/hello.c)"; exit 1; }
exec tools/build_mwp.sh "$1"
