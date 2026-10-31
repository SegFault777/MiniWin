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
# THE TWO PROFILES (1.1-alpha-13). The .img is the MINIMAL system and has to fit a 1.44 MB floppy: the kernel and the icon catalog, an
# empty /progs. It is made first, from the image exactly as build.sh left it (so run this straight after build.sh), before the demo
# programs go into the main image. The .iso (made last, below) is the FULL system: everything in the main image -- GREETER and the SDK
# examples -- plus the live installer (apps/installer.h), and is never allowed to pass 5 MB.
FLOPPY="${IMG%.img}-floppy.img"
cp "$IMG" "$FLOPPY"
python3 tools/install_icons.py "$FLOPPY" third_party/icon-bundle >/dev/null
truncate -s 1474560 "$FLOPPY"

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
# $FLOPPY (above) is what VMware / VirtualBox want when the image is attached as a floppy (the BIOS then boots it as drive 00h; the
# loader reads it by CHS and the kernel's floppy driver, kernel/fdc.h, keeps using it).
echo "install_all: $FLOPPY (1.44 MB floppy, the minimal system)"
# The live ISO (the full system; tools/mkiso.py: El Torito, the image stamped LIVE, never over 5 MB) and a size check of both.
python3 tools/mkiso.py "$IMG" "${IMG%.img}.iso" --floppy "$FLOPPY"
