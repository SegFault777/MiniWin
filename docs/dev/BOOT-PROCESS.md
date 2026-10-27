# Boot process

> **Error codes.** Every boot error has a code and a name -- never just "disk error". Stage 1 (`boot/boot.asm`), stage 2
> (`boot/stage2.asm`), the partition chain loader (`boot/chain_mbr.asm`) and the kernel (`kernel/fatal.h`) all use this table.
> Where it applies the message adds the BIOS's own error code (`AH=`), the sector (`LBA=`) and the BIOS drive number (`DL=`).
>
> | code | name | raised by | meaning |
> |---|---|---|---|
> | `MW-B01` | `BIOS_LBA_UNSUPPORTED` | (retired in 1.1-alpha-7) | was raised for a BIOS without extended reads, e.g. the image attached as a FLOPPY (`DL=00`, seen on VMware). That case now boots: stage 1 and stage 2 fall back to CHS reads and the kernel has a floppy driver |
> | `MW-B02` | `STAGE2_READ_FAILED` | stage 1 | sector 1.. could not be read (extended read, then CHS) after three tries each with a disk reset in between |
> | `MW-B03` | `KERNEL_READ_FAILED` | stage 2 | a kernel sector could not be read even one sector at a time (`LBA` is the sector; `AH=04` near the end of the disk = disk shorter than the image) |
> | `MW-B04` | `VBE_INFO_UNAVAILABLE` | stage 2 | INT 10h AX=4F00h failed: no VESA support |
> | `MW-B05` | `VBE_NO_640x480x32_MODE` | stage 2 | the VESA mode list has no 640x480 32-bit direct-colour mode |
> | `MW-C01` | `NO_ACTIVE_PARTITION` | chain loader | an MBR-booted installation inside a partition: no partition is marked active |
> | `MW-C02` | `PARTITION_READ_FAILED` | chain loader | the active partition's first sector could not be read |
> | `MW-C03` | `PARTITION_NOT_BOOTABLE` | chain loader | the active partition's first sector has no 0x55AA signature |
> | `MW-K01` | `NOT_ENOUGH_RAM` | kernel (blue screen) | the BIOS reported less memory than the 640x480x32 back buffer needs (about 9.2 MB) |
> | `MW-K02` | `VBE_PIXEL_FORMAT_UNSUPPORTED` | kernel (blue screen) | the video mode's colour channels are not in the usual red-green-blue order |
> | `MW-K10` | `CPU_EXCEPTION_<NAME>` | kernel (blue screen) | an unexpected CPU exception (vector, error code, EIP, CS, EFLAGS and registers are shown); find the function with `nm -n build/kernel.elf` |
>
> Common BIOS `AH` codes: `01` invalid function, `02` address mark not found, `04` sector not found (past the end of the disk),
> `07` drive parameter activity failed, `09` DMA crossed a 64 KB boundary, `0A` bad sector, `0C` unsupported track,
> `20` controller failure, `40` seek failed, `80` timeout / not ready, `AA` drive not ready, `BB` undefined error.
> Before 1.1-alpha-6 stage 1, stage 2 and the chain loader all printed the same "DISK READ ERROR", so the stage that failed
> was a guess; see [VIRTUAL-MACHINES.md](../user/VIRTUAL-MACHINES.md) for what to try.

## Booting from a floppy, and BIOSes without extended reads (1.1-alpha-7)

BIOS drive 00h/01h is a floppy, and a floppy has no INT 13h extended (LBA) reads: the 1.1-alpha-6 loader answered that with
`MW-B01 BIOS_LBA_UNSUPPORTED (BIOS AH=01 DL=00)` -- which is what VMware showed when the image was attached as its floppy.
Since alpha-7 the whole chain works from a floppy:

* **Stage 1** goes straight to CHS when `DL < 80h` (and falls back to CHS after a failed extended read on a hard disk that
  starts at LBA 0): stage 2 always sits at cylinder 0, head 0, sectors 2.., on any geometry.
* **Stage 2** asks the BIOS for the geometry (INT 13h AH=08h; a 1.44 MB default if it will not say), reads the kernel a track
  piece at a time through a bounce buffer at `0x1000` (so the floppy DMA never crosses a 64 KB boundary), copies it to its
  address, and passes the geometry to the kernel at `0x50C`/`0x50D`.
* **The kernel** (`kernel/fdc.h`) drives the floppy controller and ISA DMA channel 2 for the floppy it booted from -- a
  standard 1.44 MB disk, registered as `fd1` -- reading and caching a whole 18-sector track per access (a real floppy loses a
  revolution per skipped sector otherwise). The motor stays on: there is no timer to turn it off.
* `tools/install_all.sh` writes `build/os-image-floppy.img`, the image padded to 1,474,560 bytes. Use that as the floppy image
  (a 1 MB file is not a floppy size VMware accepts everywhere).

## Making the loader survive other BIOSes (1.1-alpha-6)

QEMU's BIOS (SeaBIOS) is the most forgiving there is; the loader used to rely on behaviour other BIOSes do not promise.
Stage 2 counted the 16 kernel chunks in `CX` across INT 13h calls (a BIOS only has to preserve the registers its
documentation lists, and some change `CX`), and it filled the disk address packet once and reused it (some BIOSes write
the transfer count back into it). Now: nothing needed after an INT 13h call lives in a register, the whole packet is
rebuilt for every call, `DS`/`ES` are reloaded after every call, a failed read is retried after a disk reset (AH=0), and
then retried again with half as many sectors per call, down to one, because some BIOSes cap the count per call below 64.
Stage 1 does the same for its single read. These are the known classes of difference; whether one of them was the cause
of the VMware / VirtualBox "DISK READ ERROR" reports could not be reproduced in Bochs (whose 16-bit BIOS is the ancestor
of VirtualBox's), which boots the old loader fine -- which is why the new error messages exist: the next failure will say
what it was.

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
the icon catalog and the program loader need no change.

Once the boot disk is known, `kernel_boot()` mounts the **root volume** (`fs_boot_mount()` in `kernel/fs.h`: an MWFS at LBA
`MW_ROOTFS_LBA` of the installation, as long as the superblock says), re-reads `/docs` and `/progs` into the desktop's slot indexes
and, a little earlier, starts the USB host controllers (`usb_init()`), so a USB keyboard works as soon as the desktop is up. If the
root volume cannot be mounted the desktop still starts (without documents) and MiniShell says why; the serial port logs
`[BOOT] installation base LBA <n> on <type><k>` (`ide1`, `sata2`, ...).

## The optional chain loader (`boot/chain_mbr.asm`)

446 bytes of code that `os-inst` writes into a disk's MBR **only if bytes 0-445 are all zero**. It relocates itself to
`0x0600`, finds the first partition-table entry with status `0x80`, reads that partition's first sector to `0x7C00` and
jumps to it with `DL` and `DS:SI` set as the classic convention says. It prints `MiniWin: no active partition`,
`cannot read the partition` or `partition is not bootable` and halts otherwise. `build.sh` assembles it and
`tools/gen_chain_mbr.py` turns it into `kernel/chain_mbr.h` (committed, regenerated on every build, so it cannot drift).

## Paging (1.1-alpha-11)

After the memory manager starts, `paging_init()` (`kernel/paging.h`) switches the CPU to paged addressing with an **identity map**, so no
address the kernel uses changes. RAM below 1 GB is mapped with 4 KB pages (one table per 4 MB, about 128 KB for 128 MB), everything else up
to 4 GB (RAM above 1 GB, the frame buffer, device registers) with 4 MB pages. Protections: page 0 is not present (null pointers fault),
the kernel's code and constants (`0x10000` up to `_ro_end`, set in `kernel/link.ld`) are read-only (CR0.WP), and a guard page sits under
the kernel stack and under each program window's stack. Double faults go through a **task gate** to a TSS with its own stack (a stack
overflow cannot report itself on the stack that overflowed). The error screen shows the faulting address (CR2) for a page fault. Without PSE
(no 4 MB pages) paging is left off and the kernel runs as before. Everything is still supervisor-only and ring 0; user mode is next.
`mem` shows `Paging: on, N RO, N guards`.

## Memory the kernel lives in

Kernel image + `.bss` + stack must fit **below `0x9FC00`** (the end of conventional memory) starting at `0x10000`:
the image is loaded by real-mode BIOS calls, which cannot reach above 1 MB. `build.sh` checks both limits and prints the
remaining headroom (the kernel binary against its 512 KB budget, `.bss` against the stack guard band). See
[ROADMAP.md](../project/ROADMAP.md): at 1.1-alpha-1 there are only a few KB left, and about half of the image is the Korean font.
