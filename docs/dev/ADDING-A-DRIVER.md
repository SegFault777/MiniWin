# Adding a driver

The recipe below is the one the AHCI, SCSI, NVMe, floppy and ATAPI drivers followed. It is written for a **block device**; a network
card follows the same steps with `nic.h` instead of `blk.h`.

## 1. Know the hardware before writing code

* Find the specification and an emulator that implements it. QEMU is the test bench (`-device ich9-ahci`, `-device mptsas1068`,
  `-device nvme`, floppy `-drive if=floppy`, `-cdrom`); reading its source is a good way to learn what a guest must do (but do **not**
  copy code -- QEMU is GPL-2.0-only, this project is GPL-3.0-only; write your own from the specification).
* Decide the scope in a comment at the top of the file: what it is, what it is **not**, and what it was tested on. The existing
  drivers all start with a block like that, and `tools/gen_source_map.py` turns its first sentence into the source map.

## 2. Write `kernel/<name>.h`

* `static` functions and variables (the kernel is one translation unit), first line `SPDX-License-Identifier: GPL-3.0-only`.
* `<name>_init()` finds the hardware (PCI class/vendor, fixed ports, CMOS) and registers each disk; make it idempotent.
* `<name>_read(i, lba, buf)` / `<name>_write(i, lba, buf)` -- one 512-byte sector, `1` on success, `0` on failure.
* Polled and **bounded**: no wait without a limit. Use `delay_ms()` (PIT) where the hardware needs real time.
* Log what you found with `serial_puts("[TAG] ...")`.
* DMA: add a named region to `kernel/memmap.h` (with the assert that it fits and does not overlap), below 4 GB; wrap the hand-off to
  the device in `MW_BARRIER()`.

## 3. Wire it into the block layer

1. `kernel/blk.h`: `#include` your header; add a type to `st_type_t` (or reuse one); give it a range of device indexes
   (`BLK_<NAME>_FIRST`, a count), extend `blk_type_of`, `blk_first_of`, `blk_count_of`, the dispatch in `blk_read`/`blk_write` and the
   copy into `st_devs[]` in `st_scan()`.
2. `kernel/storage.h`: the type's name in `st_type_names[]`, `st_type_has_driver()`, and -- for read-only media -- an error code
   instead of a target.
3. MiniShell's `os-inst list` and `part list` and MiniFiles' DISKS tab already loop over every device, so nothing more is needed there.

## 4. Make the boot disk findable

`st_boot_locate()` reads the sector at `base` of every present device and compares it with the boot sector the BIOS left at `0x7C00`
(a stock image) or checks the install id (an installed system). A new bus therefore needs no extra code **if** the BIOS can boot
from it and your read works at that stage. If the BIOS can boot from the bus but the kernel cannot read it, the system starts and finds
no files -- test exactly that case.

## 5. Test it

* **Host test** (`tools/test/host_*_test.c`): if the driver has pure logic (address maths, parsing), test it on the host with the
  hardware replaced by a fake -- see `host_storage_test.c` for the technique (`#define IO_H` and substitute the port functions;
  override constants such as `MW_BOOTSECT_COPY` before including the header).
* **QEMU scenarios** in `tools/test/qemu_storage.py`: boots the image attached through your device, checks the serial `[TAG]` lines
  and the "installation base LBA 0 on <id>" line, runs `os-inst` onto a second disk of that type and boots the result.
* Check `build.sh`'s size line (kernel image <= 512 KB) and the warning count (baseline in [DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md)).

## 6. Document it

Add the row to [DRIVERS.md](DRIVERS.md), the id to [STORAGE-IDS.md](STORAGE-IDS.md), the DMA region to [MEMORY-MAP.md](MEMORY-MAP.md), an
entry in [CHANGELOG.md](../project/CHANGELOG.md), the supported-hardware line in the README, and re-run `tools/gen_source_map.py`.
Say plainly what was and was not run.
