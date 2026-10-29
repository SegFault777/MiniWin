# Roadmap and known limits

## Where 1.1-alpha-9 stands

Since alpha-8: a **real file system** (MWFS: directories, `/` and `/mnt/<id>`, unlimited documents and programs, crash-safe by
construction, `part` to format/mount/check/edit partitions); **USB** keyboards, mice (wheel) and tablets on xHCI, EHCI and OHCI;
`os-inst <id> <arch>`; a MiniShell prompt that shows the current directory and Unix/Windows-style commands; **menu bars** in MiniShell,
MiniWeb and MiniFiles; scroll bars you can drag and the mouse wheel, also in Notepad and MiniShell; the apps renamed **MINIWEB** and
**MINISHELL**. See [CHANGELOG.md](CHANGELOG.md).

## Known limits

* **Storage drivers.** Legacy ATA (PIO, LBA28: IDE disks, and SATA disks the firmware shows in IDE/compatibility mode), AHCI SATA
  (DMA), LSI Logic / Fusion-MPT SCSI, NVMe, the boot floppy and ATAPI CD/DVD (read-only). **USB storage** has no driver, so `os-inst` and `part`
  cannot reach USB disks. All of it has only been run on QEMU.
* **USB.** Keyboards, mice and tablets only; no hubs (a device behind one is invisible), no UHCI, no interrupts; verified on QEMU's
  models, not on real hardware. The EHCI-to-companion hand-off for low/full-speed devices follows the specification but could not be
  exercised on QEMU.
* **Install targets.** An existing partition of at least 1 MB (create one with `part create`), on a BIOS machine; whole-disk and
  primary-partition installs can start by themselves (the latter only behind a blank MBR), logical and GPT partitions need a boot
  manager; no UEFI loader. `os-inst` takes `i386`; `x86_64` is refused because there is no 64-bit kernel.
* **Partition editing.** MBR disks only, primary partitions 1-4; GPT, extended and logical partitions can be listed, formatted and
  mounted but not edited.
* **Disk size.** LBA28: the first 128 GB; an MWFS volume is limited to 2^32 sectors, a root volume written by `os-inst` to 8 GB.
* **Files.** Documents are limited to 4096 bytes (Notepad's buffer), programs to 24576. File names are 31 characters
  (`A-Z a-z 0-9 . _ - ,`); paths 96. No timestamps, permissions or links yet. Directories cannot be moved between volumes, and a
  directory is not removed with its contents (`rm` takes empty ones only). Old disks (alpha-8 and before) are not converted.
* **Desktop.** Up to 64 documents and 32 programs get an index entry (the desktop, Notepad, MiniFiles); MiniFiles still shows a flat list
  of `/docs` and `/progs`. Notepad's menu bar is its own older code, not the shared widget.
* **A resolved ARP entry never expires**; the file system assumes a drive writes one sector atomically and in order.
* **TLS.** TLS 1.2 only, no revocation checking, no name constraints (certificates that make either critical are refused).
* **Ring 3.** Everything runs in ring 0, with no memory protection (this was planned for alpha-9 and has not been done).

## The kernel size budget

The kernel is loaded by real-mode BIOS calls into conventional memory (`0x10000`-`0x9FC00`), so image + `.bss` must fit in
about 576 KB, and the image has a 512 KB budget. Since alpha-2 the 245,784-byte Korean font ships LZ4-compressed (62,755
bytes, `kernel/lz.h`) and is inflated to `0x7D0000` at boot: the image is about 337 KB and `.bss` has about 186 KB of
headroom, so the budget is no longer the constraint it was. Still available if it ever is: more tables inflated the
same way (`tools/gen_hangul_font.py` has the compressor), moving `.bss` tables to high memory, loading the kernel above 1 MB.

## The plan (set by the author, 2026-10)

* **1.1-alpha-12** (done, see the [changelog](CHANGELOG.md)): MiniShell that follows its window, horizontal scroll bars, the disks in
  `/mnt`, nine Settings pages, kerning.
* **1.1-alpha-13**: **ring 3 and user space** together with the kernel work it needs: better memory management and paging, a kernel that
  behaves like a real one -- **processes and a scheduler** -- and **page-table based memory protection** (one address space per program).
* **1.1-alpha-14**: the web engine: **HTML5 and CSS3 implemented completely, JavaScript in part**.
* **1.1-alpha-15**: **USB flash drives** (mass storage over the existing OHCI/EHCI/xHCI stack) and a set of the essential features
  of a modern OS.
* After alpha-15 comes **beta-1**; what follows it is not decided yet.

The notes below are older and kept for their details.

## The next alphas

**1.1-alpha-10** is done (see the [changelog](CHANGELOG.md)): the explorer-style MiniFiles, icons in the start menu, task bar, title
bars and dialogs, the memory manager, the SDK and MWP API version 2. What it did not get to:

* **Save As rebuilt like the explorer** (*Save in:*, up one level, file list, *File name:*, *Save as type:*, Save/Cancel). MiniFiles'
  tree and list code is the model for it.
* **Kerning** for the text renderer (the Latin font is monospaced; "MINIWEB" shows it).
* All UI pieces (buttons, scroll bars, fields, menus, dialogs) as proper modules, and the headers of `kernel/` out of one crowded
  folder into subdirectories.

**1.1-alpha-11** has **programs that run in a window**, **paging** with fault containment, Notepad's menus, program icons, API version 3 and SDK 2.0
(see the [changelog](CHANGELOG.md)). Still planned, in the author's words: **ring 3** (user mode: user segments and a TSS with a kernel stack,
user/supervisor bits on exactly the pages a program owns, `int 0x80` gates in front of the syscall table with pointer validation, one address
space per program -- which also lifts "one program window at a time"); the **built-in apps as dynamically loaded programs** (needs the SDK to
grow first: sockets, Hangul input, settings, a text-and-HTML view -- MiniFiles-style apps are the easiest to start with); and the web engine
work, **HTML5 and CSS3 implemented completely, JavaScript in part**. `kernel/paging.h` and `kernel/apps/progwin.h` are the base for ring 3.

Other ideas, no order: more virtual-machine / hardware compatibility (NICs: e1000e, vmxnet3, PCnet, virtio-net; storage: virtio-scsi,
VMware Paravirtual SCSI, NVMe namespaces beyond the first); USB hubs and USB storage; GPT/UEFI boot; Korean labels in MiniFiles' list.
