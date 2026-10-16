# Storage ids and devices

*`kernel/storage.h`. Nothing in that file writes to a disk; installing is `kernel/os_inst.h`.*

## Grammar

```
id      := type disk part
type    := "ide" | "sata" | "nvme" | "scsi" | "usb"        (case-insensitive)
disk    := 1 or more digits, 1-based, never 0
part    := exactly 2 digits; "00" = the whole device
```

The last two digits are always the partition; everything between the type and those two digits is the disk number.
The total number of digits must be between 3 and 8. `ide1`, `ide10`, `ide001`, `ide0`, `ide101x`, `ide1-01` and
`hda101` are all rejected (the last with "unknown storage type"). `st_format_id()` writes the canonical form back
(`st_parse_id()` followed by `st_format_id()` round-trips).

## Drivers

Only `ide` has a driver: the legacy ATA ports, PIO mode, LBA28.

| id disk number | position | ports |
|---|---|---|
| `ide1` | primary channel, master | `0x1F0` / `0x3F6` |
| `ide2` | primary channel, slave | `0x1F0` / `0x3F6` |
| `ide3` | secondary channel, master | `0x170` / `0x376` |
| `ide4` | secondary channel, slave | `0x170` / `0x376` |

Disks are numbered **by position, not by order of appearance**, so an id keeps meaning the same drive when another one is
added. A position is "present" when `IDENTIFY DEVICE` answers with a plain ATA disk with LBA support. Not offered: empty
positions (status `0x00`/`0xFF`), ATAPI drives (CD/DVD), SATA drives behind an AHCI-mode controller (those are `sata`
ids, below), disks without LBA. Sizes are capped at what LBA28 can address (2^28 - 1 sectors).

## sata ids

`sata1`, `sata2`, ... are the SATA disks (signature `0x00000101`, 512-byte sectors, LBA) behind AHCI controllers (PCI class
01:06:01), numbered **in discovery order**: controller by controller, port by port, up to 8. Unlike IDE positions this
number can shift when a disk is added or removed -- check `os-inst list` before installing. Sizes are capped at 2^28 - 1
sectors like IDE.

## Partition numbers

* **MBR.** `01`-`04` are the four primary entries by table position (a gap stays a gap: entries 1 and 3 in use are
  partitions `01` and `03`). `05`, `06`, ... are the **logical** partitions inside the extended partition in chain order
  (Linux's convention). The extended container itself is listed (as "extended") but is not installable.
  The chain walk is bounded (32 logicals) and must move strictly forward, so a corrupt or cyclic chain ends.
* **GPT.** Detected by a protective-MBR entry of type `0xEE` plus a valid `EFI PART` header at LBA 1. The number is the
  entry number (`01`-`99`; entries beyond 128 are ignored). Only 32-bit LBAs are supported: an entry with a 64-bit LBA
  or with last < first is treated as absent rather than truncated.
* A protective MBR without a valid GPT header is treated as the plain MBR it is.
* A disk with no partition table (blank, or a table-less volume) has no numbered partitions; `00` still names it.

## Resolving an id (`st_resolve`)

| result | when |
|---|---|
| `ST_E_SYNTAX` | not matching the grammar |
| `ST_E_UNKNOWN_TYPE` | letters that are not a known type |
| `ST_E_NO_DRIVER` | a known type without a driver (`sata`, `nvme`, `scsi`, `usb`) |
| `ST_E_NO_DEVICE` | disk number outside the type's range, or an empty position |
| `ST_E_NO_PARTITION` | no such partition on that disk |
| `ST_E_EXTENDED` | the partition is an MBR extended container |
| `ST_E_RANGE` | the partition table points outside the device |
| `ST_E_TOO_SMALL` | fewer than 2048 sectors |
| `ST_E_OVERLAP_BOOT` | any part of the target overlaps the installation that is running |

The checks run in this order; the first failure wins. Success fills a `st_target_t`: the ATA position, the absolute base
sector, the length, the partition record and the canonical id string.

## Which disk is "the boot disk"?

The kernel has always assumed the primary master. An installation made by `os-inst` carries a random 32-bit *install id*
in its boot sector (offset 436) next to its base sector (offset 432); the boot loader hands both to the kernel
([BOOT-PROCESS.md](BOOT-PROCESS.md)) and `st_boot_locate()` finds the position whose sector at that base carries the same
id. A stock image (id 0) is the primary master with base 0, exactly as before. If two disks carry the same id (a
clone), the first position wins, deterministically. `ata_read_sector()`/`ata_write_sector()` (used by everything in
`fs.h`) add the installation's base, so every "LBA 1029" in the layout means "1029 sectors into *this* installation".
