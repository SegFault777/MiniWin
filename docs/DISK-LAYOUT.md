# Disk layout, crash-safe saves

*`kernel/fs.h` is the authority; the map in its header comment is the one to update when the layout changes.*

One MiniWin installation is a fixed **2048-sector (1 MB)** layout. Every LBA below is **relative to the installation's
base**: 0 for an image or a whole-disk install, the partition's first sector for an installation inside a partition
(`ata_read_sector()` adds the base, see [BOOT-PROCESS.md](BOOT-PROCESS.md)).

| LBA | what |
|---|---|
| 0 | boot sector (`boot/boot.asm`), install fields at offsets 432 (base) and 436 (install id) |
| 1-4 | stage 2 (`boot/stage2.asm`): kernel loading, VBE mode search, A20, GDT, protected-mode jump |
| 5-1028 | the kernel (budget 512 KB = 1024 sectors) |
| 1029-1064 | 4 document slots, 9 sectors each (1 header + 8 data): `NEWDOC.TXT`, `NEWDOC_2.TXT`, `NEWDOC_3.TXT`, `NEWDOC_4.TXT` |
| 1065-1260 | 4 program (`.mwp`) slots, 49 sectors each (1 header + 48 data) |
| 1261-1876 | icon catalog: 56 slots x 11 sectors (1 header + 32x32 + 16x16 bitmaps; written offline by `tools/install_icons.py`) |
| 1877-1885 | document journal (1 header + 8 data) |
| 1886-1934 | program journal (1 header + 48 data) |
| 1935-2047 | free headroom (~56 KB) |

`os-inst` copies LBA 0-1028 and 1261-1876, zeroes everything else, and patches the boot sector's install fields
([OS-INSTALL.md](OS-INSTALL.md)).

## Slot headers

A slot's first sector is its header (everything else zero):

| offset | document slot | program slot |
|---|---|---|
| 0 | magic `FS_MAGIC` | magic `PROG_MAGIC` |
| 4 | length in bytes (<= 4096) | length (<= 24576) |
| 8 | name (32 bytes) | entry offset |
| 12 | | name |
| 496 | journal header only: 1 + target slot | same |
| 504 | CRC-32 of the saved bytes | same |
| 508 | commit marker `CMT1` (`0x31544D43`) | same |

A header with an impossible length is treated as damaged. A header **without** the commit marker is a *legacy* header (an
image or tool from before 1.0-rc-4): still accepted, just unverifiable. `tools/install_mwp.py` writes the commit record.

## Saves are journaled

A save used to write the header first and the data after, so a power cut in between left a valid-looking header over
half-new, half-old data. Now (`fsj_save()`):

1. zero the journal header,
2. write all data sectors into the journal area,
3. write the journal header (the slot header plus the target slot) &mdash; **the commit point**,
4. only now zero the live slot's header, copy the data in and write the live header,
5. zero the journal header again.

Power loss before step 3 leaves the old file untouched; after it, the journal holds a complete, CRC-checked copy of the
new file and `fs_doc_recover()` / `prog_recover()` finish step 4 the next time the disk is touched (they run lazily on
first access, and again after any failed save). The only thing that has to be atomic is one sector write. Either the old
or the whole new file survives, never a mix. An unusable journal (staged data failing its CRC) is discarded.

Loads are all-or-nothing: `fs_read_slot()` / `prog_load_slot()` refuse a too-small buffer, an unreadable sector or a CRC
mismatch, and distinguish "damaged" from "empty". `fs_verify_slot()` / `prog_verify_slot()` do the same check without
storing the bytes (MiniFiles uses them to show DAMAGED). Oversized saves are refused before anything is written.

CRC-32 is the standard IEEE one (the one zlib uses), computed bitwise (no table).
