#!/usr/bin/env bash
# One-shot "put everything back after build.sh" -- build.sh rebuilds
# os-image.img from scratch, which wipes the program slot (GREETER.MWP)
# and the icon catalog. Forgetting to redo both is a regression this
# project has already hit once (`apps` said "no programs installed").
# Run this straight after every ./build.sh:
#
#   ./build.sh && tools/install_all.sh
#
# Usage: tools/install_all.sh [image]     (default: build/os-image.img)
set -euo pipefail
cd "$(dirname "$0")/.."
IMG="${1:-build/os-image.img}"
# Always rebuild: a .mwp is linked for ONE fixed load address (kernel/mwp_link.ld), so a stale build/greeter.mwp
# from before that address moved would load fine and then jump into garbage. It takes a fraction of a second.
tools/build_mwp.sh programs/greeter.c
python3 tools/install_mwp.py "$IMG" 0 GREETER.MWP build/greeter.mwp
python3 tools/install_icons.py "$IMG" third_party/icon-bundle
echo "install_all: GREETER.MWP + icon catalog written to $IMG"
