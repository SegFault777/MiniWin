#!/bin/bash
set -e
cd "$(dirname "$0")"

BUILD=build
mkdir -p $BUILD

echo "[1/5] Assembling bootloader..."
nasm -f bin boot/boot.asm -o $BUILD/boot.bin

echo "[2/5] Assembling kernel entry stub..."
nasm -f elf32 kernel/kentry.asm -o $BUILD/kentry.o

echo "[3/5] Compiling C kernel..."
gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -nostdinc \
    -Wall -Wextra -O1 -c kernel/kernel.c -o $BUILD/kernel.o

echo "[4/5] Linking kernel..."
ld -m elf_i386 -T kernel/link.ld -nostdlib \
    -o $BUILD/kernel.elf $BUILD/kentry.o $BUILD/kernel.o
objcopy -O binary $BUILD/kernel.elf $BUILD/kernel.bin

echo "[5/5] Building final disk image..."
# Bootloader is exactly 1 sector (512 bytes). Kernel follows on subsequent
# sectors. The bootloader reads a fixed 1024 sectors (512KB) for the
# kernel in a 16-iteration loop of 32KB chunks (see boot/boot.asm) -- if
# kernel.bin is ever bigger than that, it would get silently truncated on
# boot, so fail the build loudly instead of shipping something broken.
KERNEL_MAX_BYTES=$((1024 * 512))
KERNEL_ACTUAL_BYTES=$(stat -c%s "$BUILD/kernel.bin")
if [ "$KERNEL_ACTUAL_BYTES" -gt "$KERNEL_MAX_BYTES" ]; then
    echo "ERROR: kernel.bin is $KERNEL_ACTUAL_BYTES bytes, exceeds the" \
         "$KERNEL_MAX_BYTES-byte (1024-sector) budget the bootloader reads." \
         "Increase KERNEL_CHUNKS in boot/boot.asm (and this check) if the" \
         "kernel needs to grow further."
    exit 1
fi

cp $BUILD/boot.bin $BUILD/os-image.img
cat $BUILD/kernel.bin >> $BUILD/os-image.img

# Pad the image out to comfortably cover the kernel's 1024-sector budget
# (LBA 1-1024) plus the file-storage area used by fs.h (4 slots of 9
# sectors each, starting at LBA 1100, ending at LBA 1135), with headroom
# for future growth. NOTE: every rebuild recreates this image from
# scratch, so rebuilding wipes any previously-saved file -- persistence
# only holds across QEMU runs that reuse the same already-built
# os-image.img.
python3 - <<'EOF'
import os
path = "build/os-image.img"
target = 512 * 1200   # covers kernel (LBA 1-1024) + file slots (LBA 1100-1135) + headroom
size = os.path.getsize(path)
if size < target:
    with open(path, "ab") as f:
        f.write(b"\x00" * (target - size))
print(f"Final image size: {os.path.getsize(path)} bytes")
EOF

echo "Build complete: $BUILD/os-image.img"
ls -la $BUILD/os-image.img
