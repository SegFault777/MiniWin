#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""mkiso.py -- builds the live ISO and checks the sizes of both artifacts (1.1-alpha-13).

    tools/mkiso.py build/os-image.img build/MiniWin.iso [--floppy build/os-image-floppy.img]

The ISO is an El Torito disc whose boot image is the stock disk image (hard-disk emulation, so the boot loader reads it through INT 13h
exactly as from a disk). The image's first sector is stamped "LIVE" + the image size in sectors (boot/boot.asm's live fields) in the
disc's copy only; the kernel (kernel/live.h) sees the stamp and runs from a RAM copy of the image. Two hard limits, enforced here:

    the .iso   at most 5 MB          (never to be exceeded)
    the .img   at most 1.44 MB       (the floppy; the stock image is 1 MB)

Needs xorriso (apt install xorriso / brew install xorriso).
"""
import os, shutil, struct, subprocess, sys, tempfile

ISO_MAX = 5 * 1024 * 1024
FLOPPY_MAX = 1474560
LIVE_FIELD_OFF = 424            # boot/boot.asm: INSTALL_FIELDS_OFF (432) - 8
LIVE_MAGIC = 0x4556494C         # "LIVE"


def add_partition_entry(data):
    """El Torito hard-disk emulation takes the disk's geometry (heads, sectors per track) from the end of the FIRST PARTITION in the
    image's MBR -- an image with an empty table (the stock image owns its whole disk) is refused by the BIOS' INT 13h. So the ISO's copy
    gets one entry covering everything after the boot sector (16 heads, 63 sectors per track, the usual translation). Only the ISO's
    copy: the installer (kernel/os_inst.h) clears this table again in the boot sector it writes to a disk."""
    total = len(data) // 512
    # SeaBIOS (and others) take the geometry LITERALLY from the END CHS of partition 0: sectors per track = its sector field, heads = its
    # head + 1, cylinders = its cylinder + 1. So the end must be the last head and last sector of the last cylinder (a first attempt that
    # wrote the true CHS of the final sector gave a 3 x 1 x 32 = 96-sector disk, and the kernel read at LBA 96 failed with AH=01).
    cyl = -(-total // (16 * 63)) - 1            # cylinders needed, minus one
    entry = struct.pack("<B3sB3sII", 0x00, bytes([0, 2, 0]), 0x83, bytes([15, 63 | ((cyl >> 2) & 0xC0), cyl & 0xFF]), 1, total - 1)
    data[446:446 + 16] = entry
    data[510:512] = b"\x55\xaa"


def main():
    args = sys.argv[1:]
    floppy = None
    if "--floppy" in args:
        i = args.index("--floppy"); floppy = args[i + 1]; del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    image, iso = args
    data = bytearray(open(image, "rb").read())
    if len(data) % 512:
        sys.exit(f"{image}: not a whole number of sectors")
    if len(data) > ISO_MAX - 262144:
        sys.exit(f"{image}: {len(data)} bytes cannot fit a 5 MB ISO with its own overhead")
    struct.pack_into("<II", data, LIVE_FIELD_OFF, LIVE_MAGIC, len(data) // 512)
    add_partition_entry(data)
    xorriso = shutil.which("xorriso")
    if not xorriso:
        sys.exit("mkiso: xorriso is not installed (apt install xorriso), so no ISO was made")
    with tempfile.TemporaryDirectory() as tmp:
        with open(os.path.join(tmp, "os-image.img"), "wb") as f:
            f.write(data)
        if os.path.exists(iso):
            os.remove(iso)
        r = subprocess.run([xorriso, "-as", "mkisofs", "-quiet", "-o", iso, "-V", "MINIWIN", "-b", "os-image.img",
                            "-hard-disk-boot", tmp], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("xorriso failed:\n" + r.stdout + r.stderr)
    size = os.path.getsize(iso)
    if size > ISO_MAX:
        os.remove(iso)
        sys.exit(f"mkiso: the ISO would be {size} bytes -- the limit is {ISO_MAX} (5 MB). Nothing was written.")
    print(f"mkiso: {iso}  {size} bytes ({size * 100 // ISO_MAX}% of the 5 MB limit), live image {len(data) // 512} sectors")
    if floppy:
        fsize = os.path.getsize(floppy)
        if fsize > FLOPPY_MAX:
            sys.exit(f"mkiso: {floppy} is {fsize} bytes -- the floppy limit is {FLOPPY_MAX}")
        print(f"mkiso: {floppy}  {fsize} bytes (limit {FLOPPY_MAX})")


main()
