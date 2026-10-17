# Changelog

## MiniWin 1.1-alpha-5 (2026-10)

* **Shutdown really powers off** (`kernel/acpi.h`): Shut Down / the Terminal's `off` paint the usual "safe to turn off"
  message, then ask the machine to power off through ACPI S5 -- RSDP -> RSDT -> FADT (PM1a/PM1b control registers, SMI_CMD
  hand-off if the chipset is not in ACPI mode) and the `_S5_` sleep type read out of the DSDT bytecode without an AML
  interpreter. That covers QEMU, VMware, VirtualBox and ordinary PCs. Only under a hypervisor (CPUID bit) it also writes the
  QEMU/Bochs (0x604, 0xB004) and VirtualBox (0x4004) power-off ports. Every table is length- and checksum-checked; a firmware
  without usable tables just leaves the message on screen and halts, as before. Restart is unchanged (keyboard controller).
* **e1000 NIC models**: besides 82540EM (0x100E, QEMU / VirtualBox "MT Desktop") the driver now takes 82545EM (0x100F: VMware's
  "e1000", VirtualBox "MT Server"), 82543GC (0x1004: VirtualBox "T Server") and 82544GC (0x100C). DHCP verified on QEMU's
  82540EM, 82545EM and 82544GC models; 82543GC has no QEMU model and is untested.
* Tests: `tools/test/qemu_power.py` (Terminal `off` powers QEMU off on i440FX and Q35; with ACPI hidden it falls back and halts
  cleanly).

Known: VMware's e1000e (82574L) and vmxnet3, VirtualBox's PCnet-FAST III and virtio-net are not handled; the VirtualBox
power-off port and the S5 path on real hardware are untested.

## MiniWin 1.1-alpha-4 (2026-10)

* **SCSI driver for LSI Logic / Fusion-MPT controllers** (`kernel/mpt.h`): the one message protocol behind VMware's "LSI
  Logic" (parallel, PCI 1000:0030) and "LSI Logic SAS" (1000:0054) adapters, VirtualBox's "LsiLogic" and "LsiLogic SAS",
  QEMU's `-device mptsas1068` and the real 1030/1064/1068 chips. Resets the IOC, runs IOC Facts / IOC Init / Port Enable
  through the doorbell handshake, then polled SCSI I/O through request frames and the reply FIFO (one 512-byte sector per
  command through a bounce buffer, SYNCHRONIZE CACHE after writes). Disks are found by probing target ids 0-15 (INQUIRY +
  READ CAPACITY(10)); direct-access devices with 512-byte blocks only. `scsi1`, `scsi2`, ... are real storage ids now
  (`os-inst list`, `os-inst scsiN00`, MiniFiles' DISKS tab), and the kernel finds a boot disk that sits behind a SCSI
  controller like it does for IDE and SATA.
* Tests: `tools/test/qemu_storage.py` gained six SCSI scenarios (boot from SCSI, two targets, `os-inst scsi200` written and
  verified through MPT, the installed disk booted by install id, IDE boot with a SCSI disk present); the desktop golden is
  pixel-identical when booted from SCSI; the three-bus topology (IDE image + AHCI + SCSI) boots.

Known: run against QEMU's mptsas1068 only. VMware and VirtualBox implement the same MPI 1.x interface and the code follows
the standard driver sequence, but neither has been run here. Not handled: BusLogic, VMware Paravirtual SCSI, virtio-scsi,
NCR/Symbios 53C8xx, MPI2 chips (SAS2/SAS3: 1000:0072 and later), LUNs other than 0, disks with non-512-byte blocks.

## MiniWin 1.1-alpha-3 (2026-10)

* **Icon pack update** (`third_party/icon-bundle/`, installed by `tools/install_icons.py`): 55 distinct icons (was 47).
  Renamed: `C#_Code` -> `CS_Code`, `C++_Code` -> `CPP_Code`, `Doc` -> `doc` (names are matched case-insensitively and
  uppercased, so the catalog name `DOC` that the kernel looks up is unchanged). Added: `CSS_Code`, `Open`, `Open2`,
  `Program`, `Refresh`, `USB_2_0`, `USB_3_0_3_1_3_2_Gen_1`, `USB_3_2_Gen_2`, and 16x16 versions of `NAS`, `FD_12`, `FD_144`,
  `FD_50`, `Download_and_install` and `Upload`. Every previously existing icon is byte-identical, so no screen changes
  (classic, dark and MiniFiles goldens still match). The 56-slot catalog now has one spare slot.

## MiniWin 1.1-alpha-2 (2026-10)

* **AHCI driver** (`kernel/ahci.h`): SATA disks behind an AHCI controller (PCI class 01:06:01) -- the way a PC in
  native/AHCI mode, VirtualBox ("SATA"), VMware ("SATA") and QEMU's q35 / `ich9-ahci` present a disk. Polled, one command
  at a time, 48-bit DMA through a bounce sector, a cache flush after every write, BIOS/OS ownership hand-off. The
  `sata` storage ids are real: `sata1` is the first disk found. Before this a MiniWin booted from such a disk loaded
  (the BIOS reads it) and then found no storage at all.
* **Block layer** (`kernel/blk.h`): one numbered device table (ide1-4, then sata1-8) and `blk_read`/`blk_write`;
  `ata_read_sector()`/`ata_write_sector()` keep their signatures and now dispatch to the boot device whatever bus it is on.
  `ata_boot_pos`/`ata_boot_base` became `blk_boot_dev`/`blk_boot_base`.
* **Finding the boot disk** now works on every bus: an installed system by its install id, a stock image by comparing
  sector 0 with the copy the BIOS left at 0x7C00 (first match in table order). `pci_scan()` runs before it.
  [BOOT-PROCESS.md](BOOT-PROCESS.md)
* `os-inst`, `os-inst list` and MiniFiles' DISKS tab list and install onto `sataN` disks.
* **Kernel size relief:** the Korean font (245,784 bytes, half the image) is stored LZ4-compressed (62,755 bytes,
  `kernel/lz.h`, `tools/gen_hangul_font.py`) and inflated to `0x7D0000` at boot, verified by a checksum. Glyphs are
  byte-identical; image 517,614 -> about 337 KB, `.bss` headroom 6.7 KB -> 186 KB.
* Tests: `run_host_font.sh` (decoder + compressed table, ASan/UBSan), `qemu_storage.py` (eight boot topologies on IDE and
  AHCI plus an `os-inst sata200` round trip), `run_host_storage.sh` 157 checks.

Known: only QEMU's AHCI has been run; VirtualBox/VMware/real hardware use the same AHCI interface but are untested here.
SATA ports behind a port multiplier, ATAPI, 4K-native disks and AHCI controllers in a RAID-mode (class 01:04) are not handled.

## MiniWin 1.1-alpha-1 (2026-10)

* **MiniFiles** (`MINIFILES.MWP`): a file manager window with a FILES tab (documents and programs with size and a real
  CRC health check, open / run, two-click delete) and a DISKS tab (IDE disks and partitions with their storage ids).
  Desktop icon, start-menu entry, task-bar button. [MINIFILES.md](MINIFILES.md)
* **`os-inst`**: `os-inst ?` (usage), `os-inst list`, `os-inst <id>` installs MiniWin on another disk or partition.
  Ids are `<type><disk><2-digit partition>` (`sata103`, `ide200` = whole disk). Confirmation required, never the running
  system, sector 0 written last, everything read back; an installation can live in a partition and on any IDE position.
  [OS-INSTALL.md](OS-INSTALL.md), [STORAGE-IDS.md](STORAGE-IDS.md), [BOOT-PROCESS.md](BOOT-PROCESS.md)
* **Storage layer** (`kernel/storage.h`, a generalised `kernel/ata.h`): four IDE positions, IDENTIFY, MBR/EBR/GPT
  partitions. The boot loader passes the installation's base LBA and install id; the kernel finds its disk by them.
* Terminal: Up/Down scroll the history.
* The project is now licensed **GPL-3.0-only** (it was MIT up to 1.0); `SPDX-License-Identifier` in every source file.
  [LICENSING.md](LICENSING.md)
* Documentation: the guides listed in [README.md](README.md).
* New test suites: `run_host_storage.sh` (153 checks, including the installer), `ui_golden_files.sh`, `qemu_osinst.sh`.
  The UI goldens were re-recorded (new desktop icon, start-menu entry, version text).

* New icon bundle (47 icons, adds the storage/drive icons): the catalog grew from 45 to 56 slots, so the write-ahead journals
  moved to LBA 1877/1886 (free headroom is now LBA 1935-2047); `build.sh` now also checks the journals against the
  catalog. MiniFiles rows show 16x16 icons. [DISK-LAYOUT.md](DISK-LAYOUT.md)

Known: kernel image 517,614 of 524,288 bytes; see [ROADMAP.md](ROADMAP.md).

## MiniWin 1.0 (2026-10)

1.0 is the hardening release: no new features after 1.0-pre-29, every finding of two bug audits fixed and each
one covered by a regression test that fails without the fix. The release candidates, in order:

| build | what was fixed |
|---|---|
| 1.0-rc-1 | **critical**: ATA status polling could hang the whole kernel; a partly read program could be executed; TCP accepted any SYN-ACK from the right 4-tuple |
| 1.0-rc-2 | **critical**: IP/UDP checksums were never verified; TCP booked segments that never left the host as sent (new `IP_SEND_*` results); NIC drivers reported success when transmission never completed (and the e1000 TX-done bit was never actually re-read) |
| 1.0-rc-3 | protocol validation: TCP RST, TLS record headers and ServerHello, ARP cache poisoning, DNS answer binding, DHCP reply binding and a real lease lifecycle (renew / rebind / expire) |
| 1.0-rc-4 | storage: crash-safe journaled saves with CRC-32, oversized files refused instead of truncated, damaged files refused instead of opened, honest "save failed" UI |
| 1.0-rc-5 | oversized HTTP responses fail instead of showing a cut-off page, "page too big" notice, HTTP token lists, critical X.509 extensions and keyUsage, URL-encoding overflow, TCP peer window |

Known limits that are documented rather than fixed: no TLS 1.3, no certificate revocation or name-constraint
enforcement (such certificates are refused when critical), resolved ARP entries never expire, the disk journal
assumes a drive writes a single sector atomically and in order, and real-world TLS chains from public sites could
only be checked against the embedded roots in the development sandbox.
