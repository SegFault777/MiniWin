# Changelog

## MiniWin 1.1-alpha-8 (2026-10)

* **Desktop grid.** All desktop icons are now one vertical list at a constant spacing: the five apps (NOTEPAD, SETTING, WEB,
  TERMINAL, MINIFILES), then a shortcut for every saved document, then one for every installed program (new `PROGRAM` icon from
  the pack), top to bottom, continuing at the top of the column to the right when the next one would not fit above the taskbar
  (`kernel/ui/icons.h` `DESK_CELL_X/Y`, `ui/desktop_files.h`). A double-click opens a document in Notepad or runs a program.
  Labels are two lines (name, extension) because names are user-chosen now. The UI test scripts' icon coordinates and all
  goldens were updated for the new layout (an intended change).
* **Notepad: Save As.** File > Save As opens a dialog with a file-name field (suggesting the current name, or `NEWDOC.TXT`);
  Enter or Save writes the document under that name into a free slot (a copy: the original stays) -- or just saves it if the
  name is its own -- Esc/Cancel/X leave everything untouched. Names are 1-31 characters of `A-Z a-z 0-9 . _ -`; a name another
  document already has is refused (case-insensitive) and the dialog stays open. Document names live in the slot header
  (`fs_doc_names[]`, `fs_name_ok`, `fs_save_slot_as`, `fs_find_by_name` in `kernel/fs.h`), so they survive reboots and show on the
  desktop, in MiniFiles, in `ls` and in the status line. The limit of 4 document slots is unchanged until the new filesystem.
* **`fetch`, fastfetch style** (Terminal): OS and version, uptime, shell, display, theme and language, CPU (brand string, measured
  MHz, load), GPU (name by PCI id, VRAM in use / total), memory (reserved / total), boot disk, IP address, document and program
  counts. `kernel/sysinfo.h` reads CPUID, the TSC against the PIT, the PCI list, the VESA controller block and the BIOS memory
  figure. "CPU load" is the share of each main-loop pass spent working rather than in the pacing spin (the kernel never halts the
  CPU, so the core itself is always busy); memory is what the kernel has reserved, not touched.
* **ATAPI CD/DVD driver** (`kernel/atapi.h`): `cd1`..`cd4`, read-only, polled PIO, 2048-byte sectors served as four 512-byte ones
  with a block cache. Verified on QEMU's `-cdrom` against a known file (byte sums of two sectors match). `os-inst` refuses a disc.
* **Version in one place** (`kernel/version.h`): the desktop hint said "1.1-ALPHA-1" for six releases; it, `ver` and `fetch` now
  follow `MW_VERSION`, and `tools/test/check_version.sh` compares it with the README and this file.
* **Logo license**: CC BY-SA 4.0 (`docs/logo/LICENSE`, LICENSING.md).
* **Documentation reorganised** into `docs/user/` (for people who run MiniWin), `docs/dev/` (for people who change it) and
  `docs/project/` (changelog, roadmap, licensing), with an index in `docs/README.md`. New user pages: getting started, desktop and
  apps, Terminal commands (with `fetch` explained), files and disks, troubleshooting (every start-up error in plain words), FAQ.
  New developer pages: contributing, drivers overview, adding a driver, memory map, `.MWP` programs, UI framework, and a
  **generated source map** (`tools/gen_source_map.py`: one line per file from its own header comment). `tools/test/check_docs.py`
  fails on a broken link or a mention of a `docs/` path that does not exist.

Not done yet from the alpha-8 list: OHCI/EHCI/xHCI (USB keyboard and mouse first), memory-management modernisation, the SDK and a
real MWP API, removing the 4 document / 4 program limits (needs the new filesystem). Not run on VMware/VirtualBox/real hardware.

## MiniWin 1.1-alpha-7 (2026-10)

* **Floppy boot (the VMware `MW-B01 BIOS_LBA_UNSUPPORTED (BIOS AH=01 DL=00)` error).** `DL=00` is BIOS drive 0: VMware had booted the
  image as a floppy, which has no extended (LBA) reads. Fixed end to end: stage 1 reads stage 2 by CHS when `DL < 80h` (and as a
  fallback after a failed extended read), stage 2 reads the kernel by CHS (geometry from INT 13h AH=08h, one track piece at a time
  through a bounce buffer, handed to the kernel at 0x50C), and the kernel has a floppy-controller driver (`kernel/fdc.h`, with
  `kernel/delay.h`, a PIT-based wait) so the system keeps running from the floppy as `fd1` (reads cached per 18-sector
  track, writes through). `build/os-image-floppy.img` (the image padded to 1.44 MB) is written by `tools/install_all.sh`.
  `MW-B01` is retired. QEMU: boot from a floppy, desktop with icons identical to the golden, a Notepad save lands on the floppy;
  not run on VMware itself.
* Tests: `qemu_storage.py` gained the three floppy scenarios.


## MiniWin 1.1-alpha-6 (2026-10)

* **Boot fixes for VMware / VirtualBox ("MiniWin: DISK READ ERROR").** Two behaviours of the loader that only QEMU's BIOS
  tolerates were removed: stage 2 kept its chunk counter in `CX` across INT 13h calls, and it reused one disk address packet
  (some BIOSes write the transfer count back into it). Now nothing needed after an INT 13h lives in a register, the packet is
  rebuilt for every call, `DS`/`ES` are reloaded, a failed read is retried after a disk reset and then with half as many sectors
  per call down to one, and stage 1 retries too. I could not reproduce the original report (Bochs, whose BIOS is VirtualBox's
  ancestor, boots the old loader fine; VMware has no emulator here), so this is a hardening against the known BIOS differences, not
  a confirmed root cause -- the new messages will say exactly what failed if it still does. [BOOT-PROCESS.md](../dev/BOOT-PROCESS.md)
* **Every boot error has a code and a name**, with the BIOS error code, sector and drive where they apply: `MW-B01
  BIOS_LBA_UNSUPPORTED` (e.g. the image attached as a floppy), `MW-B02 STAGE2_READ_FAILED`, `MW-B03 KERNEL_READ_FAILED`,
  `MW-B04`/`MW-B05` VBE, `MW-C01`-`C03` partition chain loader, `MW-K01 NOT_ENOUGH_RAM`, `MW-K02 VBE_PIXEL_FORMAT_UNSUPPORTED`.
  Verified by booting the image as a floppy and from a truncated disk.
* **CPU exceptions are caught** (`kernel/fatal.h`, IDT stubs in `kentry.asm`): the kernel had no interrupt descriptor table at
  all, so any fault was a triple fault and a virtual machine just reset. Now a blue screen shows `MW-K10 CPU_EXCEPTION_<NAME>`,
  the vector, error code, EIP and registers (also on serial).
* **NVMe driver** (`kernel/nvme.h`): NVMe SSDs (class 01:08:02; what VMware 17 and recent laptops use) as `nvme1`, ...:
  admin + one I/O queue pair, polled, 512-byte blocks through a bounce page, flush after writes, namespace 1. Boot disk location,
  `os-inst`, MiniFiles and the Terminal handle `nvme` ids. Tested on QEMU's `nvme` device (boot, two controllers, `os-inst`
  round trip, install-id boot, mixed with IDE).
* **Icon pack update, 61 icons** (was 55): new `A11Y`, `ERROR`, `HELP`, `NEXT`, `POWER`, `SEARCH`, `SECURITY`, `WARNING`; `ARMASM_Code`
  and `X86ASM_Code` are now one `ASM_Code` (with a 32x32); new sizes for `EARTH`, `PC`, `TERMINAL`. The catalog grew from 56 to
  **64 slots**, so the journals moved again (documents 1877 -> 1965, programs 1886 -> 1974; disks installed earlier keep
  working, see `kernel/fs.h`) and `os-inst` copies LBA 1261-1964.
* **Mouse cursors from the icon pack** (`third_party/icon-bundle/Images/Cursor/`, 5 PNGs): the arrow and the four resize pointers
  are now the pack's black-outline/white-fill shapes instead of the old single-colour ones (which disappeared on a black or white
  window). `tools/gen_cursors.py` generates `kernel/cursor_data.h` from the PNGs (`--check` is part of `run_host_font.sh`);
  `ui/cursor.h` draws them; the outline and fill colours are theme settings (`TH_CURSOR_OUTLINE`, `TH_CURSOR_FILL`). The resize
  cursors are a little bigger (11x7 and 7x11). All 15 + 15 + 6 UI goldens were re-recorded; the only pixels that changed are
  the cursor's.
* **The icon pack is licensed CC BY-SA 4.0** (author: SegFault777), not GPL: `third_party/icon-bundle/LICENSE` and `NOTICE.md`,
  [LICENSING.md](LICENSING.md), README.
* README: MiniWin logo at the top (6x nearest-neighbour enlargement of the supplied 128x32 PNG, no smoothing, `docs/logo/`),
  and the README text re-checked against the current version (storage drivers, shutdown, icons, boot errors, file and test
  lists, a pointer to the new [VIRTUAL-MACHINES.md](../user/VIRTUAL-MACHINES.md)).

Known: nothing here has been run on VMware, VirtualBox or real hardware. Not handled: VMware's e1000e/vmxnet3, VirtualBox's PCnet,
virtio, BusLogic, Paravirtual SCSI, NVMe namespaces other than 1 and 4K-block NVMe namespaces.

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
  [BOOT-PROCESS.md](../dev/BOOT-PROCESS.md)
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
  Desktop icon, start-menu entry, task-bar button. [MINIFILES.md](../user/MINIFILES.md)
* **`os-inst`**: `os-inst ?` (usage), `os-inst list`, `os-inst <id>` installs MiniWin on another disk or partition.
  Ids are `<type><disk><2-digit partition>` (`sata103`, `ide200` = whole disk). Confirmation required, never the running
  system, sector 0 written last, everything read back; an installation can live in a partition and on any IDE position.
  [OS-INSTALL.md](../user/INSTALLING.md), [STORAGE-IDS.md](../dev/STORAGE-IDS.md), [BOOT-PROCESS.md](../dev/BOOT-PROCESS.md)
* **Storage layer** (`kernel/storage.h`, a generalised `kernel/ata.h`): four IDE positions, IDENTIFY, MBR/EBR/GPT
  partitions. The boot loader passes the installation's base LBA and install id; the kernel finds its disk by them.
* Terminal: Up/Down scroll the history.
* The project is now licensed **GPL-3.0-only** (it was MIT up to 1.0); `SPDX-License-Identifier` in every source file.
  [LICENSING.md](LICENSING.md)
* Documentation: the guides listed in [README.md](../README.md).
* New test suites: `run_host_storage.sh` (153 checks, including the installer), `ui_golden_files.sh`, `qemu_osinst.sh`.
  The UI goldens were re-recorded (new desktop icon, start-menu entry, version text).

* New icon bundle (47 icons, adds the storage/drive icons): the catalog grew from 45 to 56 slots, so the write-ahead journals
  moved to LBA 1877/1886 (free headroom is now LBA 1935-2047); `build.sh` now also checks the journals against the
  catalog. MiniFiles rows show 16x16 icons. [DISK-LAYOUT.md](../dev/DISK-LAYOUT.md)

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
