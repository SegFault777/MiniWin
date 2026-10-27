#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Builds a loadable .mwp flat binary from one programs/*.c file --
# completely separate from build.sh (which only ever builds the
# kernel), because a .mwp isn't part of the kernel image at all: it's
# meant to be written into one of kernel/fs.h's PROG_MAX_SLOTS program
# slots on disk (see install_mwp.py, run after this script) and loaded
# at runtime by kernel/mwp.h's mwp_run(), never linked into kernel.elf.
#
# Usage: tools/build_mwp.sh programs/greeter.c [--icon icon.png] [--title "Name"] [--abi N]
#   The program's own icon, title and minimum ABI version go into a trailer after the code (tools/mwp_pack.py). Without the options
#   they are taken from the source: a file NAME.png next to NAME.c is the icon, and comment lines in the source
#       /* @mwp title: MiniCalc */      /* @mwp abi: 3 */
#   set the title and the oldest ABI version it needs (the kernel refuses to start it on an older one).
# Produces: build/greeter.mwp (a raw flat binary, no ELF header, no
#           symbol table -- exactly PROG_MAX_BYTES-or-fewer bytes of
#           machine code+data ready to be copied straight into RAM at
#           kernel/mwp.h's MWP_LOAD_ADDR and jumped into)
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <programs/name.c> [--icon icon.png] [--title \"Name\"] [--abi N]"
    exit 1
fi

SRC="$1"; shift
ICON=""; TITLE=""; ABI=""
while [ $# -gt 0 ]; do
    case "$1" in
        --icon)  ICON="$2"; shift 2 ;;
        --title) TITLE="$2"; shift 2 ;;
        --abi)   ABI="$2"; shift 2 ;;
        *) echo "build_mwp: unknown option $1"; exit 1 ;;
    esac
done
# defaults from the source: NAME.png beside NAME.c, and "@mwp title:" / "@mwp abi:" comments
[ -z "$ICON" ] && [ -f "${SRC%.c}.png" ] && ICON="${SRC%.c}.png"
[ -z "$TITLE" ] && TITLE=$(grep -oP '@mwp title:\s*\K[^*]+?(?=\s*\*/|$)' "$SRC" | head -1 || true)
[ -z "$ABI" ] && ABI=$(grep -oP '@mwp abi:\s*\K[0-9]+' "$SRC" | head -1 || true)
NAME=$(basename "$SRC" .c)
BUILD="build"
mkdir -p "$BUILD"

echo "[1/3] Compiling $SRC..."
# Same freestanding flags as build.sh's own kernel compile (-m32,
# -ffreestanding, -fno-pie, -nostdlib/-nostdinc -- no libc, no position-
# independent-code trampolines, nothing this loader doesn't already
# know how to run) -- see build.sh's own comment on why -Os over -O1.
# -ffunction-sections/-fdata-sections + the linker's --gc-sections
# matter just as much here as for the kernel: PROG_MAX_BYTES is a much
# tighter budget (24KB) than the kernel's own, so throwing away unused
# code is not optional polish, it's how a program fits at all.
gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -nostdinc \
    -Wall -Wextra -Os -ffunction-sections -fdata-sections \
    -I programs -I sdk/include -c "$SRC" -o "$BUILD/$NAME.o"

echo "[2/3] Linking $NAME..."
ld -m elf_i386 -T kernel/mwp_link.ld -nostdlib --gc-sections \
    -o "$BUILD/$NAME.elf" "$BUILD/$NAME.o"
objcopy -O binary "$BUILD/$NAME.elf" "$BUILD/$NAME.mwp"

if [ -n "$ICON$TITLE$ABI" ]; then
    echo "[+] Packing metadata (icon: ${ICON:-none}, title: ${TITLE:-none}, abi: ${ABI:-1})..."
    python3 tools/mwp_pack.py "$BUILD/$NAME.mwp" "$BUILD/$NAME.mwp.packed" ${ICON:+--icon "$ICON"} ${TITLE:+--title "$TITLE"} --abi "${ABI:-1}"
    mv "$BUILD/$NAME.mwp.packed" "$BUILD/$NAME.mwp"
fi

echo "[3/3] Checking size against PROG_MAX_BYTES..."
PROG_MAX_BYTES=$(grep -oP '#define\s+PROG_MAX_BYTES\s+\K[0-9]+' kernel/fs.h)
ACTUAL_BYTES=$(stat -c%s "$BUILD/$NAME.mwp")
echo "  $NAME.mwp: $ACTUAL_BYTES / $PROG_MAX_BYTES bytes"
if [ "$ACTUAL_BYTES" -gt "$PROG_MAX_BYTES" ]; then
    echo "ERROR: $NAME.mwp ($ACTUAL_BYTES bytes) exceeds PROG_MAX_BYTES" \
         "($PROG_MAX_BYTES bytes, see kernel/fs.h). Either trim the" \
         "program or grow PROG_MAX_BYTES there (and re-run" \
         "build.sh, since that changes the disk layout)."
    exit 1
fi

echo "Build complete: $BUILD/$NAME.mwp"
