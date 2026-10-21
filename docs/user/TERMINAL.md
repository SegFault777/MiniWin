# The Terminal

Open it with **TERMINAL** on the desktop. It is 40 characters wide and 14 lines tall; the **Up/Down** keys scroll back through what
it printed. Type a command and press Enter.

| command | what it does |
|---|---|
| `?` | list the commands |
| `peek` | list the saved documents (slot number, name, size) |
| `rd <n>` | show document number *n* (1-4) |
| `del <n>` | delete document number *n* (1-4) |
| `apps` | list the installed programs |
| `run <name>` | load and run an installed program by name (for example `run greeter`) |
| `tick` | show the date and time (in the time zone chosen in Setting) |
| `fetch` | show information about the system and the machine (below) |
| `os-inst ...` | install MiniWin onto another disk or partition (below) |
| `wipe` | clear the screen |
| `re` | restart the machine |
| `off` | power the machine off |

## `fetch`

A summary like the "fastfetch" tool on Linux, one fact per line:

* **OS** -- MiniWin and its version; **Uptime** since boot; **Shell** -- this Terminal; **Display** -- resolution; **Theme** and language.
* **CPU** -- the processor's own name (from CPUID), its clock in MHz measured against the timer, and **load**: the share of each pass
  of MiniWin's main loop spent doing real work (input, network, drawing) rather than waiting for the next frame. (MiniWin never
  sleeps the processor, so the core itself is always busy; this load is MiniWin's own busy share.)
* **GPU** -- the graphics adapter's name (by PCI id, e.g. "QEMU/Bochs Standard VGA", "VMware SVGA II") and its video memory: the part
  the screen uses / the total reported by the BIOS.
* **Memory** -- what MiniWin has set aside for itself / the total RAM the BIOS reported. (It is *reserved*, not measured use: MiniWin
  has no memory allocator to count with.)
* **Disk** -- the disk MiniWin booted from and its size; **Network** -- the IP address, or *offline*; **Files** -- how many of the
  document and program slots are in use.

Where the machine does not say something (no CPUID name, no video memory figure), `fetch` prints it as unknown rather than guessing.

## `os-inst`

Installs MiniWin onto another disk or partition. It is covered in [INSTALLING.md](INSTALLING.md); the short version: `os-inst list`
shows the disks and partitions and their ids, `os-inst <id>` starts an installation and asks you to type `YES` before it overwrites
anything. Always run `os-inst list` first -- the numbers of SATA, SCSI and NVMe disks can change when disks are added or removed.
