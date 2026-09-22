#!/bin/bash
set -e
cd "$(dirname "$0")"

BUILD=build
mkdir -p $BUILD

echo "[1/5] Assembling bootloader (2 stages)..."
# STAGE2_SECTORS is defined exactly once, here, and threaded into both
# assembler invocations via -D -- boot.asm needs it to know how many
# sectors to read stage 2 into, stage2.asm needs it to know where the
# kernel starts (LBA 1 + STAGE2_SECTORS). One number, one place to
# change it, instead of three hand-synchronized copies drifting apart
# the next time either file's size budget needs to move.
STAGE2_SECTORS=4   # 2KB -- stage2.asm currently assembles to well under
                   # one sector; this leaves 3 sectors of headroom for
                   # whatever grows there next (another VBE fallback
                   # path, say) without needing to touch this number.
nasm -f bin -DSTAGE2_SECTORS=$STAGE2_SECTORS boot/boot.asm -o $BUILD/boot.bin
nasm -f bin -DSTAGE2_SECTORS=$STAGE2_SECTORS boot/stage2.asm -o $BUILD/stage2_raw.bin

STAGE2_ACTUAL_BYTES=$(stat -c%s "$BUILD/stage2_raw.bin")
STAGE2_MAX_BYTES=$((STAGE2_SECTORS * 512))
if [ "$STAGE2_ACTUAL_BYTES" -gt "$STAGE2_MAX_BYTES" ]; then
    echo "ERROR: stage2.bin is $STAGE2_ACTUAL_BYTES bytes, exceeds the" \
         "$STAGE2_MAX_BYTES-byte ($STAGE2_SECTORS-sector) budget stage 1" \
         "reads it into (boot/boot.asm's disk read, sized by build.sh's" \
         "STAGE2_SECTORS). Bump STAGE2_SECTORS in build.sh if stage2.asm" \
         "needs to grow further."
    exit 1
fi
# Pad stage2 out to a whole number of sectors -- boot.asm's disk read
# always pulls exactly STAGE2_SECTORS sectors regardless of how much of
# the last one stage2.asm's own code actually fills, so the image needs
# that much real data there (zeros are fine; nothing ever executes past
# stage2's own final instruction).
python3 -c "
import sys
with open('$BUILD/stage2_raw.bin', 'rb') as f:
    data = f.read()
target = $STAGE2_MAX_BYTES
data = data + b'\x00' * (target - len(data))
with open('$BUILD/stage2.bin', 'wb') as f:
    f.write(data)
"
echo "  stage2: $STAGE2_ACTUAL_BYTES / $STAGE2_MAX_BYTES bytes" \
     "($STAGE2_SECTORS-sector budget)"

echo "[2/5] Assembling kernel entry stub..."
nasm -f elf32 kernel/kentry.asm -o $BUILD/kentry.o

echo "[3/5] Compiling C kernel..."
# -Os over -O1: this kernel's bottleneck has never been CPU cycles (it's
# a GUI polling a mouse a few thousand times a second, not folding
# proteins) -- it's the 512-byte-sector boot budget. -Os asks the
# compiler to optimize for exactly that trade. -ffunction-sections and
# -fdata-sections split every function and global into its own linker
# section so the --gc-sections pass below can actually find and discard
# the ones nothing calls (see the comment in kernel/link.ld for why the
# wildcard patterns there matter just as much as these two flags do).
gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -nostdinc \
    -Wall -Wextra -Os -ffunction-sections -fdata-sections -c kernel/kernel.c -o $BUILD/kernel.o

echo "[4/5] Linking kernel..."
ld -m elf_i386 -T kernel/link.ld -nostdlib --gc-sections \
    -o $BUILD/kernel.elf $BUILD/kentry.o $BUILD/kernel.o
objcopy -O binary $BUILD/kernel.elf $BUILD/kernel.bin

# The kernel's .bss (all its uninitialized globals -- the NIC ring
# buffers, the framebuffer backbuffer, the network stack's tables, all
# of it) grows upward from wherever .data ends. The bootloader's stack
# starts at a fixed address and grows *downward* from there. Nothing
# stops these two from silently overlapping as .bss grows across
# sessions of adding features -- the linker has no idea a stack even
# exists, it happily lets .bss claim any address it wants. When they do
# overlap, the very first deep-ish function call scribbles over whatever
# global happened to land at the top of .bss, which is exactly the kind
# of "works nine times out of ten, then DHCP just stops replying for no
# visible reason" bug this check exists to catch before it ships instead
# of during a late-night debugging session.
echo "[4.5/5] Checking .bss doesn't collide with the stack..."
STACK_BASE=$(grep -oP 'mov\s+esp,\s*\K0x[0-9A-Fa-f]+' boot/stage2.asm | head -1)
STACK_GUARD_BYTES=4096   # don't just barely avoid the collision -- leave the
                         # stack itself some breathing room to actually recurse
BSS_VMA_HEX=$(objdump -h $BUILD/kernel.elf | awk '/\.bss/ { print "0x" $4 }')
BSS_SIZE_HEX=$(objdump -h $BUILD/kernel.elf | awk '/\.bss/ { print "0x" $3 }')
python3 - "$STACK_BASE" "$STACK_GUARD_BYTES" "$BSS_VMA_HEX" "$BSS_SIZE_HEX" <<'EOF'
import sys
stack_base = int(sys.argv[1], 16)
guard = int(sys.argv[2])
bss_vma = int(sys.argv[3], 16)
bss_size = int(sys.argv[4], 16)
bss_end = bss_vma + bss_size
budget = stack_base - guard
print(f"  .bss spans     0x{bss_vma:06X} - 0x{bss_end:06X} ({bss_size} bytes)")
print(f"  stack starts at 0x{stack_base:06X} (guard: {guard} bytes -> budget 0x{budget:06X})")
if bss_end > budget:
    print(f"ERROR: .bss ends {bss_end - budget} bytes past the stack safety "
          f"margin. The kernel's global variables (NIC buffers, the VGA "
          f"backbuffer, the network stack's tables, etc.) now grow high "
          f"enough to collide with the stack that starts at 0x{stack_base:06X} "
          f"-- the stack would silently overwrite globals living at the top "
          f"of .bss the moment call depth got even a little deep. Fix by "
          f"shrinking something in .bss (check `nm --size-sort -S` output "
          f"for the biggest offenders) or by moving the stack base higher "
          f"in boot/boot.asm (and confirming that address is still safely "
          f"inside usable conventional memory).")
    sys.exit(1)
print(f"  OK -- {budget - bss_end} bytes of headroom before the guard band")
EOF

echo "[5/5] Building final disk image..."
# Every number below is *read out of* boot/boot.asm and kernel/fs.h
# rather than copy-pasted as a second hardcoded copy -- three
# independent magic numbers that all have to agree (the bootloader's
# read budget, this build script's size check, and where fs.h starts
# carving out file slots) is exactly how a layout quietly drifts out of
# sync the next time just one of them gets edited. Deriving them here
# means changing KERNEL_CHUNKS in boot.asm is the *only* edit needed to
# resize the kernel's budget -- this script and fs.h's own comments
# follow along automatically (fs.h's FS_BASE_LBA constant itself still
# has to be hand-updated to match, since C headers can't ask the
# assembler a question at compile time -- but this check at least
# catches it immediately if that update is forgotten).
KERNEL_CHUNKS=$(grep -oP 'KERNEL_CHUNKS\s+equ\s+\K[0-9]+' boot/stage2.asm)
SECTORS_PER_CHUNK=$(grep -oP 'KERNEL_SECTORS_PER_CHUNK\s+equ\s+\K[0-9]+' boot/stage2.asm)
KERNEL_BUDGET_SECTORS=$((KERNEL_CHUNKS * SECTORS_PER_CHUNK))
KERNEL_MAX_BYTES=$((KERNEL_BUDGET_SECTORS * 512))
KERNEL_ACTUAL_BYTES=$(stat -c%s "$BUILD/kernel.bin")

# Bootloader is now 2 stages: stage 1 is exactly 1 sector (512 bytes,
# LBA 0), stage 2 occupies STAGE2_SECTORS sectors right after it
# (LBA 1..STAGE2_SECTORS), and the kernel follows for
# KERNEL_BUDGET_SECTORS sectors after THAT -- if kernel.bin is ever
# bigger than that budget, it would get silently truncated on boot, so
# fail the build loudly instead of shipping something broken.
if [ "$KERNEL_ACTUAL_BYTES" -gt "$KERNEL_MAX_BYTES" ]; then
    echo "ERROR: kernel.bin is $KERNEL_ACTUAL_BYTES bytes, exceeds the" \
         "$KERNEL_MAX_BYTES-byte ($KERNEL_BUDGET_SECTORS-sector) budget the" \
         "bootloader reads (KERNEL_CHUNKS=$KERNEL_CHUNKS x" \
         "KERNEL_SECTORS_PER_CHUNK=$SECTORS_PER_CHUNK in boot/stage2.asm)." \
         "Increase KERNEL_CHUNKS there -- and update FS_BASE_LBA in" \
         "kernel/fs.h to start immediately after the new budget -- if the" \
         "kernel needs to grow further."
    exit 1
fi
echo "  kernel: $KERNEL_ACTUAL_BYTES / $KERNEL_MAX_BYTES bytes" \
     "($KERNEL_BUDGET_SECTORS-sector budget)"

FS_BASE_LBA=$(grep -oP '#define\s+FS_BASE_LBA\s+\K[0-9]+' kernel/fs.h)
FS_SLOT_SECTORS=$(grep -oP '#define\s+FS_SLOT_SECTORS\s+\K[0-9]+' kernel/fs.h)
FS_MAX_FILES=$(grep -oP '#define\s+FS_MAX_FILES\s+\K[0-9]+' kernel/fs.h)

# The file-storage area is meant to start on the sector right after the
# kernel's own budget ends (kernel itself starting right after stage 2)
# -- no gap, nothing wasted. If a future edit grows the kernel budget
# without moving FS_BASE_LBA to match, this is the check that notices
# before it ships as a silently-corrupt layout (the kernel spilling
# into what fs.h thinks is empty file-slot space).
KERNEL_START_LBA=$((1 + STAGE2_SECTORS))
EXPECTED_FS_BASE_LBA=$((KERNEL_START_LBA + KERNEL_BUDGET_SECTORS))
if [ "$FS_BASE_LBA" -lt "$EXPECTED_FS_BASE_LBA" ]; then
    echo "ERROR: kernel/fs.h's FS_BASE_LBA ($FS_BASE_LBA) starts before the" \
         "kernel's own budget ends (LBA $EXPECTED_FS_BASE_LBA, i.e. stage 1" \
         "+ $STAGE2_SECTORS stage-2 sectors + $KERNEL_BUDGET_SECTORS kernel" \
         "sectors) -- the kernel would silently overwrite file-slot data on" \
         "disk. Update FS_BASE_LBA in kernel/fs.h to $EXPECTED_FS_BASE_LBA" \
         "or higher."
    exit 1
fi

FS_TOTAL_SECTORS=$((FS_SLOT_SECTORS * FS_MAX_FILES))
LAST_USED_LBA=$((FS_BASE_LBA + FS_TOTAL_SECTORS - 1))
MIN_IMAGE_SECTORS=$((LAST_USED_LBA + 1))
echo "  file slots: LBA $FS_BASE_LBA-$LAST_USED_LBA" \
     "($FS_MAX_FILES slots x $FS_SLOT_SECTORS sectors)"
echo "  minimum sectors needed: $MIN_IMAGE_SECTORS" \
     "($(( MIN_IMAGE_SECTORS * 512 / 1024 ))KB)"

cp $BUILD/boot.bin $BUILD/os-image.img
cat $BUILD/stage2.bin >> $BUILD/os-image.img
cat $BUILD/kernel.bin >> $BUILD/os-image.img

# Final image size: a round 512KB (1024 sectors), not whatever number
# happened to be left over after the last feature was added. Rounding
# up to a clean power-of-two-ish size (256KB/512KB/1024KB, the same
# handful of sizes disk tools, flashers, and humans all expect) instead
# of an arbitrary "1200 sectors, for headroom" is worth the trade even
# though it means picking a target instead of just measuring one -- an
# odd size like 614400 bytes signals nothing about intent, while a round
# number reads immediately as "yes, this was chosen." Bumped up from
# 256KB to 512KB once the kernel's 11x11 Galmuri11 bitmap fonts (see
# kernel/font_latin_data.h, kernel/font_ko_data.h) pushed the kernel
# itself past the old budget -- see boot/stage2.asm's KERNEL_CHUNKS and
# kernel/fs.h's layout comment for the matching numbers. The check below
# still fails loudly if the real minimum ever grows past whichever round
# number is targeted, instead of silently truncating something.
TARGET_TOTAL_BYTES=$((512 * 1024))
TARGET_TOTAL_SECTORS=$((TARGET_TOTAL_BYTES / 512))
if [ "$MIN_IMAGE_SECTORS" -gt "$TARGET_TOTAL_SECTORS" ]; then
    echo "ERROR: the kernel + file-slot layout now needs" \
         "$MIN_IMAGE_SECTORS sectors, which no longer fits in the" \
         "$TARGET_TOTAL_SECTORS-sector (${TARGET_TOTAL_BYTES}-byte) target" \
         "image size. Bump TARGET_TOTAL_BYTES in build.sh to the next" \
         "round size (512KB, 1024KB, ...) to make room."
    exit 1
fi

python3 - "$TARGET_TOTAL_BYTES" <<'EOF'
import os, sys
path = "build/os-image.img"
target = int(sys.argv[1])
size = os.path.getsize(path)
if size < target:
    with open(path, "ab") as f:
        f.write(b"\x00" * (target - size))
elif size > target:
    # Shouldn't happen given the sector check above, but truncating
    # instead of silently shipping an oversized image is the safer
    # failure mode if it ever does.
    with open(path, "r+b") as f:
        f.truncate(target)
print(f"Final image size: {os.path.getsize(path)} bytes")
EOF

echo "Build complete: $BUILD/os-image.img"
ls -la $BUILD/os-image.img
