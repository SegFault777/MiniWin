#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Installs a bundle of 16x16/32x32 PNG icons directly into an existing
os-image.img's icon catalog (see kernel/fs.h's ICON_* constants) --
same "write the disk image offline, no in-OS installer app exists yet"
approach as tools/install_mwp.py takes for loadable programs.

Expects a directory laid out like the MiniWin icon bundle actually
supplied for this project:
    <bundle>/Images/16x16/<Name>.png
    <bundle>/Images/32x32/<Name>.png
A given <Name> doesn't have to have both sizes -- whichever's present
gets written; the other half of that slot's data area is left as
whatever was already on disk (zeroed, on a freshly-built image). Names
are matched case-insensitively between the two size directories (the
bundle's Doc.png/32x32 vs doc.png/32x32 casing is inconsistent) and
installed under the 16x16 directory's uppercased stem, since a name a
future `run`/lookup call types is far more likely to match "NOTEPAD"
than "Notepad" or "notepad" -- same ALL-CAPS convention
kernel/kernel.c already uses for NOTEPAD.MWP and friends.

Usage: tools/install_icons.py build/os-image.img <bundle-dir> [--set lite|full]

--set picks WHICH icons of the bundle go into the catalog (tools/icon_sets/<set>.txt, one name per line): "lite" (the default) is the
64 icons that fill the stock catalog -- what the 1.44 MB floppy image carries; "full" is every icon of the bundle except the ones named
..._DONTUSE, for an image whose catalog (kernel/fs.h ICON_MAX_SLOTS) has room for them (the ISO). Slots are filled in name order.
"""
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("install_icons.py needs Pillow: pip install Pillow --break-system-packages")

SECTOR = 512
ICON_MAGIC = 0x494D5733  # must match kernel/fs.h's ICON_MAGIC exactly


def read_define(path, name):
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 3 and parts[0] == "#define" and parts[1] == name:
                return int(parts[2])
    sys.exit(f"couldn't find #define {name} in {path}")


def load_rgba_bytes(png_path, expect_size):
    img = Image.open(png_path).convert("RGBA")
    if img.size != (expect_size, expect_size):
        sys.exit(f"{png_path}: expected {expect_size}x{expect_size}, got {img.width}x{img.height}")
    return img.tobytes()  # row-major, R,G,B,A per pixel -- matches kernel/fs.h's icon_load_slot() doc comment


def collect_icons(bundle_dir):
    """Returns {NAME: {16: path_or_None, 32: path_or_None}}."""
    icons = {}
    for size, dirname in [(16, "16x16"), (32, "32x32")]:
        d = os.path.join(bundle_dir, "Images", dirname)
        if not os.path.isdir(d):
            continue
        for fname in os.listdir(d):
            if not fname.lower().endswith(".png"):
                continue
            stem = fname[:-4]
            name = stem.upper()
            icons.setdefault(name, {16: None, 32: None})
            icons[name][size] = os.path.join(d, fname)
    return icons


def read_set(set_name):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon_sets", set_name + ".txt")
    if not os.path.isfile(path):
        sys.exit(f"no icon set {set_name!r}: {path} does not exist")
    with open(path) as f:
        return [l.strip().upper() for l in f if l.strip() and not l.lstrip().startswith("#")]


def main():
    args = sys.argv[1:]
    set_name = "lite"
    if "--set" in args:
        i = args.index("--set")
        if i + 1 >= len(args):
            sys.exit("--set needs a name (lite or full)")
        set_name = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(f"usage: {sys.argv[0]} <image> <bundle-dir> [--set lite|full]")
    image_path, bundle_dir = args[0], args[1]

    icon_base_lba = read_define("kernel/fs.h", "ICON_BASE_LBA")
    slot_sectors = read_define("kernel/fs.h", "ICON_SLOT_SECTORS")
    max_slots = read_define("kernel/fs.h", "ICON_MAX_SLOTS")
    name_maxlen = read_define("kernel/fs.h", "ICON_NAME_MAXLEN")

    icons = collect_icons(bundle_dir)
    wanted = read_set(set_name)
    absent = [n for n in wanted if n not in icons]
    if absent:
        sys.exit(f"icon set {set_name!r} names icons the bundle does not have: {', '.join(absent)}")
    names = sorted(wanted)                      # slots are filled in name order, so the catalog is the same on every build
    skipped = sorted(set(icons) - set(wanted))
    if skipped:
        print(f"icon set {set_name!r}: {len(names)} installed; not in this set: {', '.join(skipped)}")
    if len(names) > max_slots:
        sys.exit(f"{len(names)} icons found, but ICON_MAX_SLOTS is only {max_slots} "
                  f"-- grow ICON_MAX_SLOTS in kernel/fs.h (and re-run build.sh, since "
                  f"that changes the disk layout) or trim the bundle.")

    with open(image_path, "r+b") as img:
        for slot, name in enumerate(names):
            if len(name.encode()) >= name_maxlen:
                sys.exit(f"name {name!r} too long for ICON_NAME_MAXLEN ({name_maxlen})")

            header_lba = icon_base_lba + slot * slot_sectors
            lba_32 = header_lba + 1
            lba_16 = lba_32 + 8

            header = struct.pack("<I", ICON_MAGIC) + name.encode().ljust(name_maxlen, b"\0")
            header = header.ljust(SECTOR, b"\0")
            img.seek(header_lba * SECTOR)
            img.write(header)

            paths = icons[name]
            if paths[32]:
                data = load_rgba_bytes(paths[32], 32)
                img.seek(lba_32 * SECTOR)
                img.write(data)
            if paths[16]:
                data = load_rgba_bytes(paths[16], 16)
                img.seek(lba_16 * SECTOR)
                img.write(data)

            have = "+".join(s for s in ("32", "16") if paths[int(s)])
            print(f"  slot {slot:2d}: {name:20s} ({have})")

    print(f"Installed {len(names)} icons into {image_path} "
          f"(catalog LBA {icon_base_lba}-{icon_base_lba + max_slots * slot_sectors - 1})")


if __name__ == "__main__":
    main()
