#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Puts a compiled .mwp flat binary (see tools/build_mwp.sh) into /progs of the root filesystem of an os-image.img -- the
disk-image equivalent of what kernel/fs.h's prog_save_slot() does from inside a running kernel, run offline so a demo
program is already sitting on the disk the first time the image boots.

Since 1.1-alpha-9 programs are ordinary files of the MWFS root volume (tools/mwfs.py is the offline twin of kernel/mwfs.h);
the program's entry offset lives in the file's inode ("aux"), exactly where prog_save_slot() puts it.

Usage: tools/install_mwp.py build/os-image.img 0 GREETER.MWP build/greeter.mwp [entry_offset]
       (the slot number is accepted for compatibility with older scripts and ignored: files have names, not slots)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mwfs  # noqa: E402


def read_define(path, name):
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 3 and parts[0] == "#define" and parts[1] == name:
                return int(parts[2].rstrip("uU"), 0)
    sys.exit(f"couldn't find #define {name} in {path}")


def main():
    if len(sys.argv) not in (5, 6):
        sys.exit(f"usage: {sys.argv[0]} <image> <slot (ignored)> <NAME.MWP> <binary.mwp> [entry_offset]")
    image_path, _slot, name, bin_path = sys.argv[1:5]
    entry_offset = int(sys.argv[5]) if len(sys.argv) == 6 else 0
    lba = read_define("kernel/fs.h", "MW_ROOTFS_LBA")
    sectors = read_define("kernel/fs.h", "MW_ROOTFS_STOCK_SECTORS")
    max_bytes = read_define("kernel/fs.h", "PROG_MAX_BYTES")
    name_maxlen = read_define("kernel/fs.h", "PROG_NAME_MAXLEN")
    data = open(bin_path, "rb").read()
    if len(data) > max_bytes:
        sys.exit(f"{bin_path} is {len(data)} bytes, exceeds PROG_MAX_BYTES ({max_bytes})")
    if len(name.encode()) > name_maxlen:
        sys.exit(f"name {name!r} too long for PROG_NAME_MAXLEN ({name_maxlen})")
    v = mwfs.Vol(image_path, lba, sectors)
    v.open()
    progs = v.walk("/progs")
    if v.lookup(progs, name):
        v.unlink("/progs/" + name)                      # installing again replaces it
    v.put("/progs/" + name, data, entry_offset)
    print(f"Installed {bin_path} ({len(data)} bytes) as /progs/{name}")


if __name__ == "__main__":
    main()
