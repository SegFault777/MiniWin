# Changelog

## MiniWin 1.1-alpha-13 (2026-10)

* **A live ISO** (`build/os-image.iso`, made by `tools/mkiso.py`, never over 5 MB -- the build refuses). Boot it in a VM or on a CD and
  MiniWin runs from a RAM copy of the disc image (`[LIVE] running from RAM` on the serial port). The boot image is an El Torito
  "hard disk" image (media 4) whose partition entry ends on the last head and sector of the last cylinder: BIOSes take the emulated
  disk's geometry straight from that entry (a first try with the true CHS of the last sector gave a 96-sector disk and the kernel read
  failed with `MW-B03`). The image carries a LIVE stamp in its boot sector; `kernel/live.h` reads it.
* **The INSTALL window** (`kernel/apps/installer.h`): on a live disc an INSTALL icon sits in the bottom right corner of the desktop;
  double-click it. Step 1 lists the disks and partitions the installer accepts (the others are greyed with the reason), step 2 says what
  will be erased and wants `YES` typed, step 3 writes and reads back with a progress bar, step 4 says what happened to the boot sector
  and offers Restart. It is a program-in-a-window run by `apps/progwin.h` (new `pw_launch_fn()` starts a kernel function instead of a
  `.MWP`), so the title bar, task bar button, minimise / maximise and close come for free. It drives the same engine as the terminal's
  `os-inst <id> i386` (`kernel/os_inst.h`); `inst_gui_owns` keeps the terminal's poll off a job the window owns, and a window closed
  half-way leaves the job to the poll. A disk MiniWin was installed on is not a live disc (the LIVE stamp and the ISO's partition
  entry are not copied), so it has no INSTALL icon.
* **Two images.** The `.img` is the minimal system and fits a 1.44 MB floppy (`build/os-image-floppy.img`: kernel, icon catalog, an empty
  `/progs`); the `.iso` is the full system (GREETER, the SDK examples, the installer). `tools/install_all.sh` builds both;
  `tools/test/qemu_iso.sh` boots the ISO, installs from it (terminal and window) and boots the result.
* **Settings became the Control Panel**, Windows 95 style: a 3 x 3 grid of icons (Date/Time, Display, Keyboard, Mouse, Power, Regional
  Settings, Sounds, System, Text), double-click or Enter opens a page, a Back button, File / View / Help menus, a status line, and the
  grid re-flows (with a scroll bar) when the window is narrow.
* **A new icon bundle** (75 icons, `third_party/icon-bundle`): `tools/icon_sets/{lite,full}.txt` and `install_icons.py --set lite|full`;
  folders, MiniFiles' Copy / Cut / Paste, the Start button's logo, Paint's icon. The 16 x 16 icon cache lives on the heap (48 slots).
* **Kerning almost everywhere in the UI** (`font_width_k()` / `font_draw_k()`): task bar, icon names, title bars, buttons, MiniFiles'
  status line and truncation (by pixels), the browser's status line. MiniShell, Notepad's editor, the browser body, input fields and the
  SDK's `draw_string` keep the fixed cell.
* **Terminal accounts**: the first run asks for a root password twice (shown as stars); afterwards the prompt is `~#`; closing and
  re-opening the window gives a plain user (`~$`), `root` promotes, `user` goes back. The password is kept as 16 bytes of salt +
  SHA-256 x 1000 in `/sys/ROOT.PWD`. There is no privilege difference yet.
* **More `.bss` room**: Notepad's documents (33 KB), MiniFiles' rows (11 KB) and the TLS connection (14 KB) moved to the heap
  (`sys/boot.h boot_heap_init()`).
* Tests: `tools/test/qemu_iso.sh` (new); the host tests follow the TLS connection's move to the heap and an installed disk's cleared
  LIVE stamp; `kernel/cursor_data.h` regenerated.

## MiniWin 1.1-alpha-12 (2026-10)

* **MiniShell follows its window.** The shell was a fixed 40 x 14 grid painted in the top-left corner of whatever window it lived in:
  maximised, it left a sea of black; shrunk, the text ran past the frame. Now the rows and columns shown come from the window's size,
  prose (`?`, `cat`, wrapped messages) re-flows to the current width (never narrower than 40 columns), and the scrollback holds lines up
  to 128 columns (it lives on the heap now: 256 rows x 129 bytes, allocated by the first line printed). Eleven fixed messages that used
  to be cut off at 40 columns (the `run` line of `?` lost its `-f: full screen`) now wrap instead. The input line shows its END when
  it is longer than the field, and may be 120 characters.
* **Horizontal scroll bars** (`kernel/ui/scrollbar.h`): the scroll-bar widget lies down. A window whose content is wider than it shows
  gets a bar under the content instead of drawing past its frame: MiniShell (columns) and Settings (see below) use it. Owner ids
  `SB_H_BASE + window id`; `sb_register_h()`; `sb_drag` takes both coordinates.
* **MiniFiles: the disks live in `/mnt`.** The separate Disks view is gone; `/mnt` lists the disks and their partitions (with `MOUNTED`
  / `RUNNING` notes) next to the folder tree. A new **Mount** / **Unmount** pair joins **Install** on the tool bar (and the Tools menu)
  while `/mnt` is open; double-clicking a partition mounts it if needed and opens `/mnt/<id>`. View > Disks goes to `/mnt`, View >
  Folders to `/`. Tool-bar buttons that do not fit a narrow window are left out (every action is also in a menu).
* **Settings grows from three pages to nine**: Language, IME, Time Zone, **Display** (12/24-hour clock; desktop colour presets, captions
  follow the background), **Mouse** (pointer speed slow/normal/fast/faster with the remainder carried over, double-click time),
  **Sound** (programs' beep on/off), **Text** (kerning), **About** (version, CPU, memory, screen), **Power** (restart / shut down,
  armed by a first click). The window is 420 x 200 by default; when it is too small the sidebar and the content get scroll bars.
  Nothing is saved across a reboot yet.
* **Kerning** (`kernel/font.h`): UI text (`ko_draw_mixed_string` / `ko_string_width`: menus, buttons, labels, status line) is set with the
  letters' real ink widths and with pairs such as AV, To, LT, PA tucked together. No table: the advance is each glyph's ink width + 1
  empty column, and a pair slides together until the tightest pixel row (looking one row up and down too) keeps one empty column (two next
  to a `.` `,` `:` `i` `l`, which get a column of space on both sides), by at most 2 px. Hangul, the shell, Notepad and every fixed grid
  keep the fixed cell. On by default; Settings > Text turns it off.
* **Fix: `off` did not power the machine off** (a 1.1-alpha-11 regression, found by `tools/test/qemu_power.py`): `acpi_poweroff()` read the
  BIOS data area at 0x40E straight, and since alpha-11 page 0 is not present, so the read faulted. The page is now opened for that one read.
* Internals: `desktop_bg` / `desktop_fg`, `double_click_window`, `mouse_speed`, `clock_24h`, `sound_enabled` and `font_kerning` are now
  variables instead of constants.

## MiniWin 1.1-alpha-11 (2026-10)

* **Programs run in a window.** `run NAME.MWP` in MiniShell, a double-click on a program's desktop icon, and opening one in MiniFiles
  now start it in an ordinary window (title bar with the program's icon, minimise, maximise, close, drag, resize, taskbar button)
  instead of taking over the screen; `run -f NAME.MWP` still gives it the whole screen. The desktop, the other windows, the
  network and the mouse keep working while it runs, and a program written for the full screen runs in the window **unchanged**.
  (`kernel/apps/progwin.h`, `kernel/coro.h`.)
  * How: the program runs as a **coroutine** on its own 128 KB stack and hands the CPU back at `sys.present()`, at `sys.key_poll()`
    (when no key is waiting, or after a few polls in a row) and at `sys.sleep_ms()`; the main loop resumes it once per pass. It draws
    into a private 640x480 surface (the `backbuf` pointer is swapped while it runs, so no drawing code changed), which the window
    shows; `sys.screen_w/screen_h` are the window's client size and follow a resize. Keys go to it while its window is in front; the
    pointer arrives in window coordinates; closing the window stops the program on the spot and frees its stack, surface and memory.
  * Limits: **one program window at a time** (all programs are linked for one load address), cooperative (a program that never calls
    present/key_poll/sleep_ms freezes the OS), still ring 0 with no memory protection. These are exactly what ring 3 and paging lift.
* **MWP API version 3**: `run_flags` (`mw_windowed()` in `miniwin.h`) and `set_title`. The three SDK examples draw only their inside
  when windowed and adapt to the window's size.
* Programs' `mem_alloc` tables are per run (a window's blocks are freed when it closes); `mwp_load()` split out of `mwp_run()`.
* `mem` shows the free pages in exact KB (a leak check: run a program, close it, `mem` again).
* **Paging** (`kernel/paging.h`): the CPU runs with an identity-mapped page table. Page 0 is not present, the kernel's code and constants
  are read-only, and guard pages sit under the kernel stack and every program window's stack. A CPU exception inside a program in a
  window (null pointer, write into the kernel's code, stack overflow, divide by zero, invalid instruction) **stops that program only**:
  its window closes and the status line says what happened; a stack overflow is caught through a double-fault **task gate** with its own
  stack. The error screen shows the address of a page fault. `mem` shows the paging state. `sdk/examples/crash.c` and
  `tools/test/qemu_paging.sh` provoke and check all six. Still ring 0 and supervisor-only pages: a program can write the kernel's data.
* **Notepad has File, Edit and Help** (`kernel/apps/notepad/menus.h`; the menu bar is the shared `ui/menubar.h`): New/Save/Save As/Close;
  Undo, Cut, Copy, Paste, Delete, Select All, Time/Date; Help > Keys / About. Ctrl+Z/X/C/V/A. Selection is all-or-nothing (Notepad types at the
  end of the text). A new OS **clipboard** (`kernel/clipboard.h`) is shared with programs.
* **Programs can carry their own icon**: `tools/mwp_pack.py` appends a trailer (icon 16/32 from a PNG, title, minimum ABI) to a `.mwp`
  (`kernel/mwp_trailer.h` validates it, `kernel/mwp_meta.h` caches it); the desktop, the program window's title bar and task bar and
  MiniFiles show it. `tools/build_mwp.sh` takes `NAME.png` beside `NAME.c` and `@mwp title:` / `@mwp abi:` comments. A program file may
  be up to 32 KB. Tested on 20,000 damaged files under ASan (`tools/test/run_host_mwp.sh`).
* **MWP API version 3** gained `args` (`run NAME args`), `clipboard_get/set`, `set_size`, `set_icon`, `theme_color` (programs follow the dark
  theme) and `beep`, besides `run_flags` and `set_title`.
* **SDK 2.0** (`sdk/VERSION`): `mw_libc.h` (strings, `mw_snprintf`, `mw_isqrt`, `mw_muldiv`, sine/cosine, sort), `mw_gfx.h` (lines, circles,
  gradients, aligned and wrapped text, `mw_printf`), `mw_ui.h` (an immediate-mode toolkit: buttons, check boxes, text boxes, lists, progress
  bars, message boxes, file and clipboard helpers), and the examples **MiniCalc** (a calculator: toolkit, clipboard, its own icon, `set_size`,
  `args`), `crash`, besides `hello`, `browse` and `paint`.
* **Documents have the `doc.png` icon** on the desktop (it was `docx.png`), and **new documents are named `DOC.TXT`** (`DOC (1).TXT`, ...).
* **`tools/install_all.sh --no-demos`** (or `MW_NO_DEMOS=1`) leaves GREETER and the SDK examples out of the image.
* Not done: **ring 3** (user mode, per-program address spaces), turning the built-in apps (Notepad, Settings, MiniWeb, MiniShell, MiniFiles)
  into dynamically loaded programs, and the HTML5/CSS3/JS work -- still planned (see [ROADMAP.md](ROADMAP.md)).

## MiniWin 1.1-alpha-10 (2026-10)

* **MiniFiles is an explorer now** (`kernel/apps/minifiles.h`, rewritten). A **folder tree** on the left (+/- to open and close, the
  mount points under `/mnt` with disk icons), the **contents of the selected folder** on the right in columns -- **Name, Size, Type,
  Modified** (click a header to sort) -- a **tool bar** (up, new folder, copy, cut, paste, delete, refresh, Disks), a **status bar**
  (object count or the selected file's date, size, and the volume's free space) and the menus *File Edit View Tools Help*. It reads
  the real directories of the root file system, any depth: you can make folders, **rename** (inline, like Explorer), copy and move
  files, delete, and open a document or run a program from `/docs` and `/progs`. The old DISKS tab is **View > Disks**.
  Files get a **modified time**: MWFS stores it in the inode field `gen`, unused until now, as a FAT-style packed date and time -- the
  disk layout did not change, and files written earlier show a dash.
* **Icons in more places** (`kernel/ui/icon16.h`): one shared 16x16 cache (36 icons, loaded on first use) used by MiniFiles' lists
  and tree, the **title bars** (shrunk to 12x12 with a bilinear filter), the **task bar** buttons, the **start menu** (programs, shut
  down, restart) and the **warning dialog**.
* **Notepad names new documents `DOC.TXT`** (then `DOC (1).TXT`, `DOC (2).TXT`, ...), not `NEWDOC.TXT`. For that, MWFS names may
  now contain spaces and parentheses (never a leading or trailing space), the Save As field accepts them, and **MiniShell takes
  double-quoted words** (`cat "DOC (1).TXT"`).
* **Memory manager** (`kernel/mm.h`): a page-frame allocator and a heap (`kmalloc kzalloc krealloc kfree`) over everything above
  `0x900000` (118 MB on a 128 MB machine). Double frees, frees of pointers that are not blocks, and smashed block headers are
  refused and counted; `mm_check()` walks the whole heap. New MiniShell command **`mem`** (`mem test` runs a self-check). Tested on
  the host with the address and undefined-behaviour sanitizers (`tools/test/run_host_mm.sh`, 29,647 checks). Not paging: there is
  still no memory protection.
* **A real SDK** (`sdk/`): `sdk/include/miniwin.h` and `mwp_abi.h`, `sdk/build.sh`, `sdk/install.sh`, three example programs
  (`hello`, `browse`, `paint`, installed on every image) and `sdk/README.md`. The syscall table now has **one definition**
  (`mwp_abi.h`, read by the kernel and by programs).
* **MWP API version 2** (`kernel/mwp_api2.h`): memory (`mem_alloc`/`mem_free`, released automatically when the program returns),
  files and folders (`file_size file_read file_write file_delete dir_make dir_list err_text`), time (`ticks_ms sleep_ms time_now`),
  `mouse_poll`, icons, `text_width`, `bevel`, `button`, `window_frame`, `log`, `rand32`. Version 1 programs run unchanged.
  `key_poll` now also drains the mouse: the keyboard and mouse share one controller output buffer, and a pending mouse byte used to
  make a program deaf to the keys.
* **Single-pass directory reads** (`mwfs_dir_each`, `vfs_ls_each`): listing a folder no longer rereads it once per entry.
* **Not done, moved on:** Save As in the explorer style, kerning, and the split of the UI widgets and headers into subfolders (see
  [ROADMAP.md](ROADMAP.md)). Ring 3 and windowed programs are planned for alpha-11. Nothing here was run on real hardware.

## MiniWin 1.1-alpha-9 (2026-10)

Slogan, from this version on: **Mini OS, Many Things.**

* **A real file system: MWFS** (`kernel/mwfs.h`, `kernel/vfs.h`). Directories of any depth, files that grow (up to about 8 MB), a
  free-space bitmap, a CRC-32 per file, names of up to 31 characters, any number of documents and programs -- the four-document
  and four-program limits are gone. `/` is the root volume (`/docs`, `/progs`, `/mnt`, `/sys`); other volumes appear as `/mnt/<id>`.
  No journal any more: a save builds the new file in a fresh inode and one directory-entry write switches the name over, so a power
  cut leaves the old file or the whole new one, and at worst garbage that `part check <id> fix` collects. Tested on a RAM disk
  (`tools/test/run_host_mwfs.sh`, 6216 checks) including a crash sweep of every operation at every possible sector write, a
  random run checked against a model, and an image built by the Python twin (`tools/mwfs.py`). **The disk layout changed**
  (kernel, then the icon catalog at LBA 1029, then the root volume at LBA 1733); disks written by alpha-8 and before are not
  converted, their documents are not carried over. `kernel/fs.h` keeps the slot API as an index of `/docs` (64) and `/progs` (32).
* **MiniShell** (the Terminal, renamed). The **prompt shows the current directory** (`/docs~# `). New commands with the familiar
  names: `ls cd pwd mkdir touch cat cp mv rm` (Windows `dir md type copy move del` and short MiniWin aliases `go look where dig new
  dup` work too), `del <path>`, `rd <file>`, `clear` (`cls`, `wipe`), `reboot` (`re`), `shutdown` (`poweroff`, `off`), `lsusb` (`plug`).
  **`part`** with `list info format mount unmount check create delete set merge` (destructive ones ask `YES`; partition tables are
  only written on MBR disks, primary entries 1-4, never over the running system). **`os-inst <id> <arch>`** takes `i386`
  (`x86_64` is refused: there is no 64-bit kernel); the installer now copies the system sectors and **formats** a fresh root
  filesystem over the rest of the target (up to 8 GB). The command parser takes up to five words.
* **USB** (`kernel/usb/`): **xHCI, EHCI and OHCI** host controller drivers and a HID driver: boot keyboards (with key repeat),
  mice and "USB tablets" (absolute pointers, as VirtualBox/VMware present by default) through a small report-descriptor parser,
  the mouse wheel, hot-plug. Verified on QEMU with the PS/2 controller removed (`-machine pc,i8042=off`). Not implemented: hubs,
  UHCI, USB storage. The EHCI-to-companion hand-off is written to the specification but QEMU cannot exercise it. Nothing was run on
  VMware, VirtualBox or real hardware.
* **Mouse wheel and scroll bars.** One scroll bar widget (`ui/scrollbar.h`): arrows, a thumb you can **drag**, page jumps, and the wheel
  (PS/2 IntelliMouse and USB). **Notepad** and **MiniShell** now have scroll bars (Notepad scrolls its text and follows the typing);
  MiniWeb and MiniFiles use the same widget.
* **Menu bars** in MiniShell (*Shell, View, Tools, Help*), MiniWeb (*Page, Sites, Help*) and MiniFiles (*File, View, Help*), each
  fitted to the app, built on a shared `ui/menubar.h`.
* **Renamed:** WEB.MWP -> **MINIWEB.MWP**, Terminal -> **MINISHELL.MWP** (desktop, title bars, status line, start menu, docs).
* **Fixed:** an IDE disk of 32 MB or more was reported as 128 GB (the IDENTIFY words 100-103 were misread); a machine without an
  i8042 controller (its ports read all ones) made the mouse driver loop forever.
* **Tests:** host suites 134 + 300 fuzz + 952 + 204 + 22 + 166 + 6216 + 15 checks, all passing; `qemu_osinst.sh` and the UI goldens
  were updated for the new layout. The old journal / torn-write tests were retired with the journals (their job is `host_mwfs_test.c`).
* **Documentation reviewed** end to end for this release: disk layout, files and disks, MiniShell, installing, MiniFiles, desktop and
  apps, drivers (USB), memory map, UI framework, testing, roadmap, README.

Not done from the alpha-9 list: **ring 3** (user mode and memory protection) -- it moves to alpha-10. Also planned for alpha-10:
an explorer-style MiniFiles and Save As, kerning, a full modularisation of the UI pieces and of the `kernel/` headers, memory
management and kernel modernisation, the SDK and a richer MWP API, icons in more places (see [ROADMAP.md](ROADMAP.md)).

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
