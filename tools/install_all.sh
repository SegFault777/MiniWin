#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# One-shot "put everything back after build.sh" -- build.sh rebuilds
# os-image.img from scratch, which wipes the root filesystem (GREETER.MWP in /progs)
# and the icon catalog. Forgetting to redo both is a regression this
# project has already hit once (`apps` said "no programs installed").
# Run this straight after every ./build.sh:
#
#   ./build.sh && tools/install_all.sh
#
# Usage: tools/install_all.sh [--no-demos] [image]     (image default: build/os-image.img)
#   --no-demos   leave out the demo and example programs (GREETER and everything in sdk/examples): the image gets only the icon
#                catalog, and /progs starts empty. MW_NO_DEMOS=1 in the environment does the same.
set -euo pipefail
cd "$(dirname "$0")/.."
NO_DEMOS="${MW_NO_DEMOS:-0}"
IMG="build/os-image.img"
for a in "$@"; do
    case "$a" in
        --no-demos) NO_DEMOS=1 ;;
        --demos)    NO_DEMOS=0 ;;
        -h|--help)  sed -n 11,15p "$0"; exit 0 ;;
        -*)         echo "install_all: unknown option $a (see --help)"; exit 1 ;;
        *)          IMG="$a" ;;
    esac
done
if [ "$NO_DEMOS" != "1" ]; then
    # Always rebuild: a .mwp is linked for ONE fixed load address (kernel/mwp_link.ld), so a stale build/greeter.mwp
    # from before that address moved would load fine and then jump into garbage. It takes a fraction of a second.
    tools/build_mwp.sh programs/greeter.c
    python3 tools/install_mwp.py "$IMG" 0 GREETER.MWP build/greeter.mwp

    # The SDK's example programs (sdk/examples): they show the version-2 API and double as a smoke test of it on every build.
    for ex in hello:HELLO browse:BROWSE paint:PAINT minicalc:MINICALC crash:CRASHTST; do
        src="${ex%%:*}"; name="${ex##*:}"
        tools/build_mwp.sh "sdk/examples/$src.c"
        python3 tools/install_mwp.py "$IMG" 0 "$name.MWP" "build/$src.mwp"
    done
fi
python3 tools/install_icons.py "$IMG" third_party/icon-bundle
if [ "$NO_DEMOS" = "1" ]; then echo "install_all: icon catalog written to $IMG (demos left out)"; else echo "install_all: GREETER.MWP + the SDK examples + icon catalog written to $IMG"; fi
# The same image padded to a 1.44 MB floppy: what VMware / VirtualBox want when the image is attached as a floppy (the BIOS then boots
# it as drive 00h; the loader reads it by CHS and the kernel's floppy driver, kernel/fdc.h, keeps using it). A raw copy, nothing else.
FLOPPY="${IMG%.img}-floppy.img"
cp "$IMG" "$FLOPPY" && truncate -s 1474560 "$FLOPPY"
echo "install_all: $FLOPPY (1.44 MB floppy copy)"
