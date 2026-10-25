# Files, programs and disks

## Directories, documents and programs

MiniWin has a real file system, **MWFS**: directories inside directories, files that can be created, copied, moved and deleted,
and as many documents and programs as the disk has room for (the four-document limit of the early versions is gone).

* `/` is the system's own disk. Its standard folders are `/docs` (your documents), `/progs` (programs, the `.MWP` files), `/mnt`
  (other disks, see below) and `/sys`.
* **MiniShell** is where you move around: `ls`, `cd`, `pwd`, `mkdir`, `cat`, `cp`, `mv`, `rm`, `touch`. The prompt always shows
  where you are (`/docs~#`). See [TERMINAL.md](TERMINAL.md).
* Notepad saves into `/docs`; the documents (up to 64 of them are shown) appear as desktop icons and in MiniFiles. A document can
  be up to 4 KB, a program up to 24 KB.
* Programs are put into a disk image *before* it is started, with the tools in `tools/` (see [MWP-PROGRAMS.md](../dev/MWP-PROGRAMS.md)),
  or copied into `/progs` with `cp`; they appear as desktop icons, in MiniFiles and in `apps`, and run with a double-click or `run <name>`.
* **Names:** up to 31 characters from `A-Z a-z 0-9 . _ - ,`, not starting with a dot; capital letters do not matter when you type a name.
* **Power cuts:** a save writes the new version completely before the name is switched over, so a cut leaves the old file or the new
  one, never a mixture; at worst some unreachable leftovers stay behind, and `part check <id> fix` gives them back.
  A file whose bytes no longer match what was saved (a failing disk) is refused instead of opened.

## Other disks: `part`

Any partition or whole disk MiniWin can see can get its own MWFS volume and be attached under `/mnt`:

    part list                  # what disks and partitions there are
    part format ide202         # an empty MWFS volume (asks YES: everything on it is erased)
    part mount ide202          # it now appears as /mnt/ide202
    cp /docs/NOTE.TXT /mnt/ide202
    part unmount ide202

`part` can also look at a partition (`info`), check a volume (`check`, `check ... fix`), and on MBR disks create, delete, set the
active flag or type of, and merge partitions. Details in [TERMINAL.md](TERMINAL.md). Partition tables are only ever written on MBR
disks, only for primary partitions 1-4, and never for anything overlapping the system MiniWin is running from.

## Disks MiniWin understands

MiniWin starts from any of these and finds the disk it started from by itself:

| kind | examples |
|---|---|
| IDE hard disk (`ide1`..`ide4`) | QEMU's default disk, VMware/VirtualBox "IDE" |
| SATA / AHCI (`sata1`...) | VirtualBox "SATA", VMware "SATA", QEMU `-device ich9-ahci` |
| SCSI, LSI Logic (`scsi1`...) | VMware "LSI Logic" / "LSI Logic SAS", VirtualBox "LsiLogic" |
| NVMe SSD (`nvme1`...) | VMware 17 NVMe, QEMU `-device nvme` |
| floppy (`fd1`) | the 1.44 MB floppy image, when MiniWin was started from it |
| CD/DVD (`cd1`...) | any IDE CD/DVD drive with a disc (read-only) |

USB **keyboards, mice and tablets** work (see below); USB sticks and USB drives are **not** supported yet. The **ids** (`sata2`,
`nvme1`, ...) name a disk; two more digits name a partition (`sata203` = partition 3 of the second SATA disk, `sata200` = the whole
disk). The full rules are in [STORAGE-IDS.md](../dev/STORAGE-IDS.md).

## USB input

MiniWin drives the three kinds of USB host controller: **xHCI** (USB 3), **EHCI** (USB 2) and **OHCI** (USB 1.1; also the
companion that low- and full-speed devices behind an EHCI controller are handed to), and the keyboard, mouse and "USB tablet"
(absolute pointer) devices on them, with the mouse wheel. They are found at start-up and hot-plugging is noticed. `lsusb` lists what
was found. Not supported: USB hubs (a device behind one is invisible), Intel UHCI controllers, USB mass storage, and everything that
is not a keyboard or pointer.

## Installing MiniWin onto another disk

See [INSTALLING.md](INSTALLING.md). **It erases what is on the target** (after you type `YES`), so read `os-inst list` carefully.

## Network cards

Intel **e1000** family (QEMU's `e1000`, VMware "e1000", VirtualBox "Intel PRO/1000" cards) and **Realtek RTL8139**. The network is
configured automatically (DHCP). Not supported: VMware vmxnet3 and e1000e, VirtualBox PCnet and virtio-net.
