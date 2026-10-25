# Disk layout and MWFS, the MiniWin file system

*`kernel/fs.h` is the authority for where things sit on the disk, `kernel/mwfs.h` for the file system's format; the map in
`fs.h`'s header comment is the one to update when the layout changes, and `build.sh` checks it.*

One MiniWin installation is laid out like this. Every LBA below is **relative to the installation's base**: 0 for an image
or a whole-disk install, the partition's first sector for an installation inside a partition (`ata_read_sector()` adds the
base, see [BOOT-PROCESS.md](BOOT-PROCESS.md)). The stock image is exactly **2048 sectors (1 MB)**.

| LBA | what |
|---|---|
| 0 | boot sector (`boot/boot.asm`), install fields at offsets 432 (base) and 436 (install id) |
| 1-4 | stage 2 (`boot/stage2.asm`): kernel loading, VBE mode search, A20, GDT, protected-mode jump |
| 5-1028 | the kernel (budget 512 KB = 1024 sectors) |
| 1029-1732 | the icon catalog: 64 slots x 11 sectors (1 header + 32x32 + 16x16 bitmaps; written offline by `tools/install_icons.py`, never by the running OS) |
| 1733-2047 | the **root volume**, `/`: an MWFS of 315 sectors in the stock image (`MW_ROOTFS_LBA`, `MW_ROOTFS_STOCK_SECTORS`) |

`os-inst` copies LBA 0-1732 (patching the boot sector's install fields) and **formats** a fresh root volume that fills the rest
of the target partition or disk, up to 8 GB; see [INSTALLING.md](../user/INSTALLING.md). `build.sh` fails if the kernel's
budget overlaps the catalog, the catalog overlaps the root volume, or the root volume does not end exactly at the image's end.

Before 1.1-alpha-9 the area after the kernel held four fixed document slots, four program slots and two write-ahead journals.
That layout is gone, and so are the disks written in it: an old disk is not converted, its documents are not carried over.

## MWFS

A small real file system: directories of any depth, files that grow, a free-space bitmap. It lives inside any run of sectors:
the root volume above, or a whole partition or disk formatted with `part format`. All sizes are 512-byte sectors, all offsets
relative to the start of the volume.

| sector | what |
|---|---|
| 0 | **superblock**: magic `MWF2`, version, total sectors, inode count, bitmap/inode-table/data start, root inode, label, CRC-32. Written *last* by mkfs, so a half-formatted volume has none. |
| `bitmap_lba` | free-space bitmap, 1 bit per sector of the volume (1 = in use, metadata included) |
| `itable_lba` | inode table: 64-byte inodes, 8 per sector, inode 0 unused, inode 1 = the root directory. Inode count = sectors/8, clamped to 32..16384 |
| `data_lba` | file data, directories, indirect blocks |

An **inode** holds the type (free/file/dir), size, the CRC-32 of a file's bytes, one free 32-bit field (programs keep their entry
offset there), 8 direct sector pointers, one single-indirect and one double-indirect pointer: a file can reach 8 + 128 +
128x128 sectors (about 8 MB). A **directory** is a file of 64-byte entries (inode, type, name up to 31 characters of
`A-Z a-z 0-9 . _ - ,`, not starting with a dot), 8 per sector. Names are case-insensitive and keep the case they were given.

### How crash safety works (there is no journal)

A save never changes a file in place. The new content is written into a freshly allocated inode and its sectors; only when it is
complete does **one directory-entry sector write** switch the name over to it; the old inode is freed afterwards. Cut the power
before that write and the old file is intact, after it the new one is. Directory edits (create, remove, rename) are single-sector
writes as well, and a rename writes the new name before removing the old, so an interruption leaves the file under both names,
never under none. What a crash *can* leave behind is garbage: an inode or sectors nobody points at. `part check <id>` finds it
and `part check <id> fix` gives it back (`mwfs_check()`: every entry must point at a live inode of the right type, every
sector must be inside the data area and marked used, every file must match its CRC; unreachable inodes are orphans, used
sectors nobody owns are leaks). The check refuses to call anything an orphan if its directory walk was incomplete. The one
assumption, as before, is that a drive writes a single sector atomically and in order.

Loads are all-or-nothing: `fs_read_slot()` / `prog_load_slot()` refuse a too-small buffer, an unreadable sector or a CRC mismatch.

`tools/test/run_host_mwfs.sh` tests all of this on a RAM disk, including a **crash sweep**: every operation is cut off after k
sector writes for every k, and the volume must come back with each file either old or new, nothing referenced but unmarked, and a
clean volume after `check fix`. `tools/mwfs.py` is the offline twin used by `build.sh` to format the stock image's root volume
and by `tools/install_mwp.py` to put programs in it; the host test opens an image built by it with the C code.

## The namespace, and the "slots" that are left

`kernel/vfs.h` gives the volumes one namespace: `/` is the root volume, `/mnt/<id>` any mounted volume (`ide200`, `sata101`, ...),
paths are absolute or relative to the shell's current directory, may use `.` and `..`, and nest as deep as the 96-character path
buffer allows. `/mnt` is a real, empty directory on the root volume; what `ls /mnt` shows is the mount table, which exists only in
RAM (mounting is a choice, not a setting). The root volume has `/docs`, `/progs`, `/mnt` and `/sys`.

`fs.h` keeps the **slot API** the desktop, Notepad, MiniFiles and the shell were written against, as a thin layer: slot *n* is
the *n*-th entry of a cache of `/docs` (up to `FS_MAX_FILES` = 64) or `/progs` (up to `PROG_MAX_SLOTS` = 32), so a window that
points at "document 3" keeps working while the bytes live in ordinary files. A document is still at most
`FS_MAX_FILE_BYTES` = 4096 bytes because that is what Notepad's buffer holds; a program is at most `PROG_MAX_BYTES` = 24576.
`fs_rescan_docs()` / `fs_rescan_progs()` re-read the directories (boot does, and so do the shell commands that change them).
