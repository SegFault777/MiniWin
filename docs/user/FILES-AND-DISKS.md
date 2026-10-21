# Files, programs and disks

## Documents and programs

MiniWin stores documents and programs in fixed **slots** on its own disk (a simple layout that survives power cuts: every save is
journalled, so a cut in the middle leaves either the old or the new version). Today there are **four document slots** (4 KB each)
and **four program slots** (24 KB each). Documents have names you choose (Notepad's *Save As*); programs have the name they were
installed with. The limit of four is a property of the current disk layout and is planned to disappear with a new file system.

* Documents appear as desktop icons and in MiniFiles; **Terminal:** `peek`, `rd`, `del`.
* Programs (`.MWP` files) are installed onto a disk image *before* it is started, with the tools in `tools/` (see
  [MWP-PROGRAMS.md](../dev/MWP-PROGRAMS.md)); they appear as desktop icons, in MiniFiles and in `apps`, and run with a double-click
  or `run <name>`.
* **MiniFiles** can delete a document or program; it asks first.

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

USB sticks and USB drives are **not** supported yet. The **ids** (`sata2`, `nvme1`, ...) name a disk; two more digits name a
partition (`sata203` = partition 3 of the second SATA disk). The full rules are in [STORAGE-IDS.md](../dev/STORAGE-IDS.md).

## Installing MiniWin onto another disk

See [INSTALLING.md](INSTALLING.md). **It erases what is on the target** (after you type `YES`), so read `os-inst list` carefully.

## Network cards

Intel **e1000** family (QEMU's `e1000`, VMware "e1000", VirtualBox "Intel PRO/1000" cards) and **Realtek RTL8139**. The network is
configured automatically (DHCP). Not supported: VMware vmxnet3 and e1000e, VirtualBox PCnet and virtio-net.
