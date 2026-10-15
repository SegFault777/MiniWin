# Changelog

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
