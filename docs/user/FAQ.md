# Frequently asked questions

**Does it run on a real PC?**
It is only tested on QEMU so far. The code is written for real hardware (legacy BIOS boot, IDE/AHCI/NVMe/SCSI/floppy drivers,
ACPI power-off), but a PC needs a legacy BIOS or a UEFI "CSM" mode, and nothing here has been verified on real machines yet. Try it
from a virtual machine first.

**Does it run in VMware / VirtualBox?**
That is what it is being prepared for; see [VIRTUAL-MACHINES.md](VIRTUAL-MACHINES.md). Neither has been run by the developer so far -- reports
with the error name and numbers are very welcome.

**Does it support USB (keyboard, mouse, drives)?**
Not yet. The keyboard and mouse are the old PS/2 kind, which virtual machines emulate. USB keyboard/mouse support is the next
hardware goal; USB storage comes after.

**Is it 64-bit?**
No, it is a 32-bit (i386) system. A 64-bit version is planned for later; there is no 64-bit kernel yet.

**Can it run Windows or Linux programs?**
No. It runs its own tiny `.MWP` programs, written in C against a small API ([MWP-PROGRAMS.md](../dev/MWP-PROGRAMS.md)).

**How many documents and programs can I keep?**
As many as the disk has room for: MiniWin has a real file system (directories, files that grow, other disks mounted under `/mnt`; see
[FILES-AND-DISKS.md](FILES-AND-DISKS.md)). A document is limited to 4 KB because that is what Notepad holds, a program to 24 KB. The
desktop shows the first 64 documents and 32 programs.

**Is my data safe if the power goes out while saving?**
A save writes the new version completely before the name is switched over, so after a power cut you get either the complete old
document or the complete new one, not a mixture; at worst some unreachable leftovers remain, and `part check <id> fix` gives them
back. (Tested with simulated power cuts at every sector of every operation.)

**Does it work with a USB keyboard and mouse?**
Yes: xHCI, EHCI and OHCI controllers are driven, with keyboards, mice (including the wheel) and "USB tablets"; `lsusb` shows what was
found. USB hubs, USB sticks/drives and Intel UHCI controllers are not supported yet.

**Can I install it onto a real disk?**
`os-inst` can write MiniWin onto another disk or partition that MiniWin can see (IDE, SATA, SCSI, NVMe). It is careful -- it refuses
to overwrite the disk it is running from and asks you to type `YES` -- but it **does overwrite** the target. See [INSTALLING.md](INSTALLING.md).

**What are the licenses?**
The code is GPL-3.0-only. The icon pack and the logo are CC BY-SA 4.0. The fonts and the root certificates keep their own licenses.
Details: [LICENSING.md](../project/LICENSING.md).

**Where do I report a bug or ask a question?**
<https://github.com/SegFault777/MiniWin/issues>.
