# Roadmap and known limits

## Where 1.1-alpha-6 stands

New in alpha-2: an AHCI driver (`sata` ids), a block layer, boot-disk location on every bus, and the Korean font stored
compressed (kernel image ~337 KB of 512 KB). See [CHANGELOG.md](CHANGELOG.md). New in alpha-1: MiniFiles ([MINIFILES.md](MINIFILES.md)), `os-inst` ([OS-INSTALL.md](OS-INSTALL.md)) with the storage
layer under it ([STORAGE-IDS.md](STORAGE-IDS.md), [BOOT-PROCESS.md](BOOT-PROCESS.md)), Terminal scrollback (Up/Down), the GPL-3.0
license ([LICENSING.md](LICENSING.md)).

## Known limits

* **Storage drivers.** Legacy ATA (PIO, LBA28: IDE disks, and SATA disks the firmware shows in IDE/compatibility mode) and AHCI
  SATA (DMA; disks capped at the first 128 GB like IDE) and LSI Logic / Fusion-MPT SCSI (VMware/VirtualBox "LSI Logic",
  polled). USB storage is recognised in storage ids but has no driver, so `os-inst` cannot reach them and a
  MiniWin installed on one could not run. AHCI and MPT have only been run on QEMU.
* **Install targets.** An existing partition (MiniWin cannot create or resize partitions) of at least 1 MB, on a BIOS
  machine; whole-disk and primary-partition installs can start by themselves (the latter only behind a blank MBR),
  logical and GPT partitions need a boot manager; no UEFI loader.
* **Disk size.** LBA28: the first 128 GB.
* **Files.** Four document slots and four program slots, no folders, no rename/copy/move. Documents are limited to 4096
  bytes, programs to 24576.
* **A resolved ARP entry never expires**; the disk journal assumes a drive writes one sector atomically and in order.
* **TLS.** TLS 1.2 only, no revocation checking, no name constraints (certificates that make either critical are refused).

## The kernel size budget

The kernel is loaded by real-mode BIOS calls into conventional memory (`0x10000`-`0x9FC00`), so image + `.bss` must fit in
about 576 KB, and the image has a 512 KB budget. Since alpha-2 the 245,784-byte Korean font ships LZ4-compressed (62,755
bytes, `kernel/lz.h`) and is inflated to `0x7D0000` at boot: the image is about 337 KB and `.bss` has about 186 KB of
headroom, so the budget is no longer the constraint it was. Still available if it ever is: more tables inflated the
same way (`tools/gen_hangul_font.py` has the compressor), moving `.bss` tables to high memory, loading the kernel above 1 MB.

## Ideas for the next alphas

* **next:** more virtual-machine / hardware compatibility (NICs: e1000e, vmxnet3, PCnet, virtio-net); more storage buses
  (virtio-scsi, VMware Paravirtual SCSI, NVMe namespaces beyond the first); a partition-table tool (`os-part`?); MiniFiles: rename/copy, more slots.
* **later:** NVMe and USB storage; GPT/UEFI boot; reading a disk image into MiniFiles; per-file permissions are out of scope.
* Translate MiniFiles' labels and messages to Korean.
