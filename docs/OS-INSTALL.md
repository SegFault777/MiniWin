# Installing MiniWin onto a disk: `os-inst`

*Added in 1.1-alpha-1. Implementation: `kernel/os_inst.h` (the job), `kernel/storage.h` (devices, partitions, ids),
`kernel/apps/terminal.h` (the command), `boot/chain_mbr.asm` (the optional loader).*

`os-inst` copies the running MiniWin onto another disk or partition, so that machine (or virtual machine) can boot
MiniWin on its own. It is a Terminal command:

```
os-inst ?          explain it (usage, id format)
os-inst list       show the IDE disks and their partitions, each with the id to use
os-inst <id>       install onto <id>
```

## The storage id

```
<type><disk number><partition number, 2 digits>
```

| part | meaning |
|---|---|
| type | `ide`, `sata`, `nvme`, `scsi` or `usb` |
| disk number | 1, 2, 3 ... counted within the type |
| partition | `01`, `02`, `03` ... the partition with that number; **`00` = the whole disk** |

Examples: `sata103` = the 1st SATA disk, partition 3. `ide200` = the whole 2nd IDE disk. `ide302` = partition 2 of the
3rd IDE position. The last two digits are always the partition, so `nvme1205` is disk 12, partition 05.
Full rules and the error table are in [STORAGE-IDS.md](STORAGE-IDS.md).

**This build drives IDE, SATA (AHCI), SCSI (LSI Logic / MPT) and NVMe disks.** `ide1`/`ide2` are the primary channel's
master/slave, `ide3`/`ide4` the secondary channel's; `sata1`, `sata2`, ... and `scsi1`, `scsi2`, ... are numbered in the
order the disks are found (see [STORAGE-IDS.md](STORAGE-IDS.md)), so run `os-inst list` first. The other type name
(`usb`) is recognised (so `nvme101` gets a precise answer rather than "syntax error") and reported as "no driver
in this build yet".

## A session

```
> os-inst list
ide1  1 MB  QEMU HARDDISK
  <- MiniWin is running from here
  no partition table
  whole disk: ide100
ide2  16 MB  QEMU HARDDISK
  no partition table
  whole disk: ide200
ide3  (nothing)
ide4  (nothing)
usb: no driver yet
> os-inst ide200
target: ide200  (whole disk)
  QEMU HARDDISK, 16 MB
  starts at sector 0
  it looks empty
It writes 1 MB there and erases it.
Type YES to install; else cancels.
> YES
  installing... do not power off
  checking 95%
  done: written and verified
  installed on ide200
  boot that disk to use it
```

(The Terminal scrolls with the Up/Down keys; the usage text is longer than the window.)

## What is written

One complete installation is the fixed 2048-sector (1 MB) layout described in [DISK-LAYOUT.md](DISK-LAYOUT.md),
placed at the target's first sector (sector 0 for a whole disk, the partition's first sector for a partition):

* the **boot sector** with two patched fields: where this installation starts, and a fresh random *install id*;
* **stage 2 and the kernel**, and the **icon catalog**, copied from the disk MiniWin booted from;
* **zeros** for everything else (document slots, program slots, journals): the new system starts with *no user data*,
  and stale bytes in the target can never be mistaken for a file.

Only those 2048 sectors are written; the rest of a larger partition or disk is not touched.

## Safety rules

1. **Vetted target.** The id must resolve to a device that exists, a real (non-extended) partition inside the device,
   at least 1 MB large.
2. **Never the running system.** If any part of the target overlaps the installation MiniWin is running from, it is
   refused. That includes `00` on the disk that holds the running partition, because installing to a whole disk
   rewrites sector 0 and with it the partition table.
3. **You confirm.** The target, its model and size, its first sector, whether it looks empty or already holds data (or a
   MiniWin), and how many partitions a whole-disk install would destroy are printed; nothing happens until you type
   `YES` exactly. Anything else cancels. (MiniFiles' INSTALL button only *types the command for you* in the Terminal.)
4. **Sector 0 last.** The sector that makes the area bootable is written *last*, so an install that fails or is
   interrupted never looks bootable, and when installing to a whole disk the old partition table survives until the
   very last write.
5. **Read back.** Every sector is read back and compared before the install is declared successful.
6. **Other people's boot loaders are never overwritten.** See below.
7. **A background job.** The install advances a few sectors per main-loop pass, so the screen keeps updating; while it
   runs the Terminal refuses other commands. Powering off mid-install is safe in the sense of rule 4: the target is
   simply not bootable.

## Booting the result

* **Whole disk (`<type><n>00`).** The disk now has MiniWin's own boot sector as its MBR: boot that disk.
* **A partition.** The partition's first sector is MiniWin's boot sector (it knows where the partition starts).
  Something has to start it:
  * If the disk's MBR **code area (bytes 0-445) is completely blank**, `os-inst` writes the small loader from
    `boot/chain_mbr.asm` there and marks the target partition active, keeping the rest of the partition table (the
    other partitions' active flags are cleared: exactly one partition is active). The disk then boots MiniWin by
    itself.
  * If the MBR already has **any** boot code, it is **left alone** and `os-inst` says so: point your boot manager at the
    partition (chainload its first sector).
  * A **logical** partition or a **GPT** disk cannot be started by the built-in loader (it boots the active *primary*
    partition of a classic MBR); the install is still complete, `os-inst` says so, and a boot manager can chainload it.
* The disk may be on **any IDE position**: at boot the kernel looks for the disk whose boot sector carries its own install
  id ([BOOT-PROCESS.md](BOOT-PROCESS.md)), so a MiniWin installed from `ide2` still boots when it is later the only disk,
  on the primary or the secondary channel.

## What `os-inst` does not do

* It does not create or resize partitions: make the partition with another tool first (the id list shows what exists).
* It does not copy your documents or programs (a fresh system is empty by design).
* It needs a BIOS-bootable machine (legacy/CSM boot); there is no UEFI loader.
* It reaches disks through the legacy ATA ports: an IDE disk, or a SATA disk the firmware exposes in IDE/compatibility
  mode. AHCI-only SATA, NVMe, SCSI and USB storage are the next drivers to write ([ROADMAP.md](ROADMAP.md)).
* LBA28: only the first 128 GB of a disk are addressable.

## Messages

| message | meaning |
|---|---|
| `bad id: expected <type><device><2-digit partition>` | the id is not `<type><digits>` with at least a disk digit and 2 partition digits |
| `this build has no driver for that storage type yet (only ide, sata, scsi, nvme)` | `usb` |
| `no such device` / `no such partition on that device` | see `os-inst list` |
| `that is an extended-partition container ...` | choose a logical partition inside it |
| `target is smaller than 1 MB` | too small for a 2048-sector installation |
| `that area is where MiniWin is running from right now` | rule 2 |
| `INSTALL FAILED ... the target is not bootable now` | a write or the read-back failed; the failing sector is shown |
