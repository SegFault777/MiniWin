# Boot process

## Stages

1. **BIOS** loads the first sector of the boot disk to `0x7C00` and jumps there with `DL` = the BIOS drive number.
2. **Stage 1** (`boot/boot.asm`, exactly 512 bytes). Saves `DL` at `0x0500`. Copies the install fields from its own sector
   into the boot info block (below). Reads **stage 2** (4 sectors) at `install_base + 1` with `INT 13h` extensions
   (AH=42h, a disk address packet) and jumps to it.
3. **Stage 2** (`boot/stage2.asm`, at `0x0600`). Walks the BIOS VBE mode list for a 640x480x32 mode, probes RAM
   (`INT 15h E801`), enables A20, loads the GDT, then reads the **kernel** at `install_base + 5` in 64-sector chunks to
   physical `0x10000` (1024 sectors = 512 KB, ending at `0x90000`), and switches to protected mode.
4. **Kernel** (`kernel/kernel.c`): `kernel_boot()` first locates its own disk (below), then initialises everything else.

Both loader stages add `install_base` as a **32-bit** value: a partition's LBAs routinely exceed 65535.

## Boot info block (low memory)

| address | size | content | written by |
|---|---|---|---|
| `0x0500` | 1 byte | BIOS boot drive number | stage 1 |
| `0x0504` | u32 | **installation base LBA** (0 = the installation owns the whole disk) | stage 1, from `install_base` |
| `0x0508` | u32 | **install id** (0 = a stock image never installed by `os-inst`) | stage 1, from `install_id` |

## The install fields

Inside the 512-byte boot sector, at **offset 432** (`INSTALL_FIELDS_OFF`): `install_base` (u32), then `install_id` (u32).
432 stays clear of the MBR disk signature (440-445) and the partition table (446+). They are zero in a freshly built
image; **only `os-inst` ever writes them**. A build-time `times` directive in `boot.asm` stops the build if the code ever
grows past offset 432.

## Finding the boot disk

The kernel reaches disks through the legacy ATA ports (`kernel/ata.h`), AHCI SATA ports (`kernel/ahci.h`) and LSI Logic / MPT SCSI controllers (`kernel/mpt.h`), put in one
device table by `kernel/blk.h`. `kernel_boot()` scans PCI, then calls `st_boot_locate(base, id)` (`kernel/storage.h`)
before anything touches the disk. The BIOS can boot from any bus it has a driver for, and the kernel has to find the same
disk again by itself; it walks the table in order (ide1-4, then sata1, sata2, ..., then scsi1, scsi2, ...) and takes the first disk whose sector
at `base` matches:

* `id != 0` (an installed system): offsets 432/436 equal `(base, id)`.
* `id == 0` (a stock image): the sector equals the copy of the boot sector the BIOS left at `0x7C00`, compared on the
  first 48 bytes (code) and bytes 432-511 (install fields, partition table, signature); `boot.asm` rewrites its own
  disk-address packet in between, so the middle is not comparable.

If nothing matches (booted from a bus this build cannot drive) the kernel falls back to ide1 with the base applied, and
logs it. The result is the pair `blk_boot_dev` / `blk_boot_base`; `ata_read_sector()` and `ata_write_sector()` use them, so
the filesystem, the icon catalog and the program loader need no change. The serial port logs
`[BOOT] installation base LBA <n> on <type><k>` (`ide1`, `sata2`, ...).

## The optional chain loader (`boot/chain_mbr.asm`)

446 bytes of code that `os-inst` writes into a disk's MBR **only if bytes 0-445 are all zero**. It relocates itself to
`0x0600`, finds the first partition-table entry with status `0x80`, reads that partition's first sector to `0x7C00` and
jumps to it with `DL` and `DS:SI` set as the classic convention says. It prints `MiniWin: no active partition`,
`cannot read the partition` or `partition is not bootable` and halts otherwise. `build.sh` assembles it and
`tools/gen_chain_mbr.py` turns it into `kernel/chain_mbr.h` (committed, regenerated on every build, so it cannot drift).

## Memory the kernel lives in

Kernel image + `.bss` + stack must fit **below `0x9FC00`** (the end of conventional memory) starting at `0x10000`:
the image is loaded by real-mode BIOS calls, which cannot reach above 1 MB. `build.sh` checks both limits and prints the
remaining headroom (the kernel binary against its 512 KB budget, `.bss` against the stack guard band). See
[ROADMAP.md](ROADMAP.md): at 1.1-alpha-1 there are only a few KB left, and about half of the image is the Korean font.
