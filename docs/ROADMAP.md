# Roadmap and known limits

## Where 1.1-alpha-1 stands

New in this release: MiniFiles ([MINIFILES.md](MINIFILES.md)), `os-inst` ([OS-INSTALL.md](OS-INSTALL.md)) with the storage
layer under it ([STORAGE-IDS.md](STORAGE-IDS.md), [BOOT-PROCESS.md](BOOT-PROCESS.md)), Terminal scrollback (Up/Down), the GPL-3.0
license ([LICENSING.md](LICENSING.md)).

## Known limits

* **Storage drivers.** Only legacy ATA (PIO, LBA28): IDE disks, and SATA disks that the firmware shows in IDE/compatibility
  mode. AHCI-only SATA, NVMe, SCSI and USB storage are recognised in storage ids but have no driver, so `os-inst` cannot
  reach them and a MiniWin installed on one could not run (the kernel itself reads its disk through these ports).
* **Install targets.** An existing partition (MiniWin cannot create or resize partitions) of at least 1 MB, on a BIOS
  machine; whole-disk and primary-partition installs can start by themselves (the latter only behind a blank MBR),
  logical and GPT partitions need a boot manager; no UEFI loader.
* **Disk size.** LBA28: the first 128 GB.
* **Files.** Four document slots and four program slots, no folders, no rename/copy/move. Documents are limited to 4096
  bytes, programs to 24576.
* **A resolved ARP entry never expires**; the disk journal assumes a drive writes one sector atomically and in order.
* **TLS.** TLS 1.2 only, no revocation checking, no name constraints (certificates that make either critical are refused).

## The kernel size budget (the constraint that shapes the next releases)

The kernel is loaded by real-mode BIOS calls into conventional memory (`0x10000`-`0x9FC00`), so image + `.bss` + stack must
fit in about 576 KB, and the image has a 512 KB budget. At 1.1-alpha-1 about **6.5 KB** of each remain. Of the 517,614-byte
image, **245,784 bytes are the Korean font bitmap** (`font8x8_ko`), nearly half of it. Options, cheapest first:

1. Move the large `.bss` tables that never need to be low (`tls_conn` ~14 KB, the Notepad text buffers ~17 KB) to the
   high-memory regions `memmap.h` already uses for network and engine buffers: a one-line macro per table plus zeroing at boot.
2. Store the Korean font in a disk area (there is headroom at LBA 1935-2047, but only 56 KB of it, so 240 KB needs a layout change) and load
   it into high memory at boot with the ATA driver that is already there; this frees almost half the image.
3. Load the kernel above 1 MB (protected-mode loader), removing the 640 KB ceiling altogether.

## Ideas for the next alphas

* **alpha-2:** an AHCI driver (SATA in native mode), the `sata` ids becoming real; a partition-table tool (`os-part`?);
  MiniFiles: rename/copy, more slots; the kernel-size relief above (option 1 and 2 first).
* **later:** NVMe and USB storage; GPT/UEFI boot; reading a disk image into MiniFiles; per-file permissions are out of scope.
* Translate MiniFiles' labels and messages to Korean.
