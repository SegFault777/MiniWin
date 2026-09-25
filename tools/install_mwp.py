#!/usr/bin/env python3
"""Writes a compiled .mwp flat binary (see tools/build_mwp.sh) directly
into one of kernel/fs.h's PROG_MAX_SLOTS program slots on an existing
os-image.img -- the disk-image equivalent of what kernel/fs.h's own
prog_save_slot() does from inside a running kernel, just run offline so
a demo program can already be sitting on disk the very first time the
image boots, without needing some in-OS "install" UI that doesn't exist
yet.

This writes the EXACT SAME header layout prog_save_slot() writes (magic
+ len + entry_offset + name, in that byte order -- see kernel/fs.h's
own comment on PROG_MAGIC for why it's a different constant than the
document slots' FS_MAGIC) so that prog_check_slot() reading this back
from inside the OS can't tell the difference between a program placed
here and one saved by the OS itself at runtime. If kernel/fs.h's header
layout ever changes, this script's HEADER struct below has to change to
match, by hand -- there's no shared source of truth between C and
Python here, same trade-off as mwp_api.h/mwp.h's syscall table staying
in sync by hand.

Usage: tools/install_mwp.py build/os-image.img 0 GREETER.MWP build/greeter.mwp
       (slot 0, name "GREETER.MWP", entry_offset defaults to 0)
"""
import struct
import sys

SECTOR = 512
PROG_MAGIC = 0x50575732  # must match kernel/fs.h's PROG_MAGIC exactly


def read_define(path, name):
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 3 and parts[0] == "#define" and parts[1] == name:
                return int(parts[2])
    sys.exit(f"couldn't find #define {name} in {path}")


def main():
    if len(sys.argv) not in (5, 6):
        sys.exit(f"usage: {sys.argv[0]} <image> <slot> <NAME.MWP> <binary.mwp> [entry_offset]")

    image_path, slot_s, name, bin_path = sys.argv[1:5]
    entry_offset = int(sys.argv[5]) if len(sys.argv) == 6 else 0
    slot = int(slot_s)

    prog_base_lba = read_define("kernel/fs.h", "PROG_BASE_LBA")
    slot_sectors = read_define("kernel/fs.h", "PROG_SLOT_SECTORS")
    data_sectors = read_define("kernel/fs.h", "PROG_DATA_SECTORS")
    max_slots = read_define("kernel/fs.h", "PROG_MAX_SLOTS")
    name_maxlen = read_define("kernel/fs.h", "PROG_NAME_MAXLEN")

    if not (0 <= slot < max_slots):
        sys.exit(f"slot {slot} out of range (0..{max_slots - 1})")

    with open(bin_path, "rb") as f:
        data = f.read()

    max_bytes = data_sectors * SECTOR
    if len(data) > max_bytes:
        sys.exit(f"{bin_path} is {len(data)} bytes, exceeds PROG_MAX_BYTES ({max_bytes})")
    if len(name.encode()) >= name_maxlen:
        sys.exit(f"name {name!r} too long for PROG_NAME_MAXLEN ({name_maxlen})")

    header_lba = prog_base_lba + slot * slot_sectors
    data_lba = header_lba + 1

    # Same 512-byte header layout as fs.h's prog_save_slot(): magic(u32
    # LE) + len(u32 LE) + entry_offset(u32 LE) + name (NUL-padded to
    # PROG_NAME_MAXLEN bytes), zero-padded out to a full sector.
    header = struct.pack("<III", PROG_MAGIC, len(data), entry_offset)
    header += name.encode().ljust(name_maxlen, b"\0")
    header = header.ljust(SECTOR, b"\0")

    with open(image_path, "r+b") as img:
        img.seek(header_lba * SECTOR)
        img.write(header)

        img.seek(data_lba * SECTOR)
        img.write(data)
        # Pad the rest of this slot's data sectors with zeros so a
        # shorter program doesn't leave a previous, longer program's
        # tail bytes sitting there unread (harmless today since
        # prog_load_slot() only ever reads back `len` bytes, but leaving
        # stale bytes on disk that nothing accounts for is exactly the
        # kind of "works today, mysterious tomorrow" debt this project
        # tries not to leave lying around).
        img.write(b"\0" * (max_bytes - len(data)))

    print(f"Installed {bin_path} ({len(data)} bytes) as {name!r} "
          f"in program slot {slot} (LBA {header_lba}-{header_lba + slot_sectors - 1})")


if __name__ == "__main__":
    main()
