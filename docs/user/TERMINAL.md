# MiniShell

Open it with **MINISHELL** on the desktop. It is 40 characters wide and 14 lines tall; the **Up/Down** keys, the mouse wheel and
the scroll bar scroll back through what it printed. Type a command and press Enter.

The **prompt shows where you are**: `/docs~# ` means you are in the directory `/docs`, `/mnt/ide202/photos~# ` in a folder of a
mounted disk. A long path keeps its end (`.../x/y/z~# `).

The menu bar has **Shell** (clear the screen, the command list, close), **View** (scroll), **Tools** (disks, the directory you are in,
USB devices, the time) and **Help**; every Tools item simply types the command for you.

## Files and directories

Paths are absolute (`/docs/NOTE.TXT`) or relative to the current directory, may use `.` and `..`, and are not case-sensitive. The names
are the familiar Unix ones; the Windows ones work too (shown in brackets), and MiniWin's own short names are accepted as well.

| command | what it does |
|---|---|
| `ls [dir]` (`dir`, `look`) | list a directory (`/` shows `docs/ progs/ mnt/ sys/`; `ls /mnt` shows the mounted disks) |
| `cd <dir>` (`go`) | change directory; `cd` alone goes to `/`; `cd ..` goes up |
| `pwd` (`where`) | show the current directory |
| `mkdir <dir>` (`md`, `dig`) | make a directory (the parent must exist) |
| `touch <file>` (`new`) | make an empty file if it does not exist |
| `cat <file>` (`type`) | show a text file, up to 4 KB (a binary file is only described) |
| `cp <a> <b>` (`copy`, `dup`) | copy a file; `cp a /docs` copies into the directory; works between disks |
| `mv <a> <b>` (`move`) | move or rename; between disks a file is copied and then removed; directories cannot cross disks |
| `rm <path>` (`del <path>`) | delete a file or an **empty** directory. There is no undo. `/`, `/mnt` and the mounted disks cannot be removed |

An existing file at the destination of `cp` or `mv` is replaced (the replacement is crash-safe: you get the old file or the new one).

## Documents and programs

| command | what it does |
|---|---|
| `peek` | list the saved documents (number, name, size) |
| `rd <n>` or `rd <file>` | show document number *n* (see `peek`), or any file by path |
| `del <n>` | delete document number *n*; `del <path>` is the same as `rm` |
| `apps` | list the installed programs |
| `run <name>` | start an installed program **in a window** (for example `run hello`); `run -f <name>` gives it the whole screen. Only one program window at a time; close it with its X button or let the program end (ESC in the examples) |

## System

| command | what it does |
|---|---|
| `?` | list the commands |
| `tick` | show the date and time (in the time zone chosen in Setting) |
| `fetch` | show information about the system and the machine (below) |
| `lsusb` (`plug`) | list the USB controllers and the keyboards, mice and tablets on them |
| `part ...` | partitions and file systems (below) |
| `os-inst <id> [<arch>]` | install MiniWin onto another disk or partition (below) |
| `clear` (`cls`, `wipe`) | clear the screen |
| `reboot` (`re`) | restart the machine |
| `shutdown` (`poweroff`, `off`) | power the machine off |

## `fetch`

A summary like the "fastfetch" tool on Linux, one fact per line:

* **OS** -- MiniWin and its version; **Uptime** since boot; **Shell** -- MiniShell; **Display** -- resolution; **Theme** and language.
* **CPU** -- the processor's own name (from CPUID), its clock in MHz measured against the timer, and **load**: the share of each pass
  of MiniWin's main loop spent doing real work (input, network, drawing) rather than waiting for the next frame. (MiniWin never
  sleeps the processor, so the core itself is always busy; this load is MiniWin's own busy share.)
* **GPU** -- the graphics adapter's name (by PCI id, e.g. "QEMU/Bochs Standard VGA", "VMware SVGA II") and its video memory: the part
  the screen uses / the total reported by the BIOS.
* **Memory** -- what MiniWin has set aside for itself / the total RAM the BIOS reported. (It is *reserved*, not measured use: MiniWin
  has no memory allocator to count with.)
* **Disk** -- the disk MiniWin booted from and its size; **Network** -- the IP address, or *offline*; **Files** -- how many documents and programs there are.

Where the machine does not say something (no CPUID name, no video memory figure), `fetch` prints it as unknown rather than guessing.

## `part`

Look at, format, mount, check and edit partitions. `<id>` is the same id `os-inst` uses: `ide101`, `sata200` (`00` = the whole
disk); see [STORAGE-IDS.md](../dev/STORAGE-IDS.md). The commands that destroy something print what they will do and wait for you to
type `YES`; anything else cancels.

| command | what it does |
|---|---|
| `part list` | every disk, its partitions, what is on them (`mwfs`, `mwfs (MiniWin)`, `[mounted]`) |
| `part info <id>` | size, start, type, and for an MWFS volume its label and free space |
| `part format <id>` | lay a fresh, empty MWFS volume over it (asks `YES`; erases it). Refuses the running system's disk and anything mounted |
| `part mount <id>` | attach the volume as `/mnt/<id>`. A partition holding an installed MiniWin mounts its root volume |
| `part unmount <id>` | detach it again (`cd` out of it is done for you) |
| `part check <id> [fix]` | verify the file system; with `fix` give back the leftovers of a crash. The running root volume is checked read-only |
| `part create <id> [mb]` | new primary MBR partition `01`-`04` in free space (the largest gap, or the first one big enough for *mb* megabytes) |
| `part delete <id>` | remove an MBR entry (asks `YES`; the data stays on the disk but becomes unreachable) |
| `part set <id> active\|inactive` | the bootable flag of an MBR entry (only one partition stays active) |
| `part set <id> type <hex>` | change the type byte (not to an extended-partition type) |
| `part merge <id1> <id2>` | join two *neighbouring* MBR partitions of one disk into one (asks `YES`; both are erased) |

Partition tables are only written on **MBR** disks, only primary entries 1-4, and never for anything overlapping the system MiniWin runs
from. GPT disks and extended/logical partitions can be listed, formatted and mounted, not edited. A new partition is marked type `DA`
("non-file-system data"); format it with `part format` to use it.

## `os-inst`

Installs MiniWin onto another disk or partition. It is covered in [INSTALLING.md](INSTALLING.md); the short version: `os-inst list`
shows the disks and partitions and their ids, `os-inst <id> <arch>` starts an installation (`<arch>` is `i386`, the 32-bit x86 this kernel is; `x86_64` is refused with an explanation because there is no 64-bit kernel yet; it may be left out) and asks you to type `YES` before it overwrites
anything. Always run `os-inst list` first -- the numbers of SATA, SCSI and NVMe disks can change when disks are added or removed.
