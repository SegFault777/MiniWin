# Changelog

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
