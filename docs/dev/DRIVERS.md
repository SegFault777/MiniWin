# Drivers

Every driver is a `static` header in `kernel/` included by `kernel.c` in dependency order (see [ARCHITECTURE.md](ARCHITECTURE.md)). All of
them are **polled** -- the kernel never enables interrupts, so there are no interrupt handlers, no IRQ routing and no
deferred work; a driver waits (with a bounded loop) for the hardware to finish. Status below is for 1.1-alpha-8; "tested" always means
*on QEMU's emulation of that device* unless stated -- no driver has been run on VMware, VirtualBox or real hardware yet.

## Storage (everything behind `blk.h`)

`blk.h` keeps one numbered table of block devices and dispatches `blk_read`/`blk_write` (absolute LBA, one 512-byte sector) to the
driver. `ata_read_sector()`/`ata_write_sector()` -- what the file system uses -- talk to the **boot device**, found by
`st_boot_locate()` ([BOOT-PROCESS.md](BOOT-PROCESS.md)). Ids and numbering: [STORAGE-IDS.md](STORAGE-IDS.md).

| id | file | hardware | how it is found | notes and limits |
|---|---|---|---|---|
| `ide1`-`ide4` | `ata.h` | legacy IDE/ATA (ports 0x1F0/0x170) | fixed ports, IDENTIFY | PIO, LBA28; the oldest, simplest driver |
| `cd1`-`cd4` | `atapi.h` | ATAPI CD/DVD on the IDE channels | IDENTIFY signature, IDENTIFY PACKET | read-only, 2048-byte sectors served as four 512-byte ones with a one-block cache; no ISO 9660 yet |
| `sata1`-`sata8` | `ahci.h` | SATA behind an AHCI controller (PCI 01:06:01) | PCI scan, ABAR (BAR5) | 48-bit DMA through a bounce sector, flush after every write; no NCQ, ATAPI, port multipliers or 4K-native disks |
| `scsi1`-`scsi8` | `mpt.h` | LSI Logic / Fusion-MPT (VMware/VirtualBox "LSI Logic", QEMU `mptsas1068`) | PCI vendor 1000 | MPI 1.x; doorbell handshake, then polled request frames; targets 0-15, LUN 0, 512-byte blocks |
| `nvme1`-`nvme4` | `nvme.h` | NVMe SSD (PCI 01:08:02) | PCI scan, BAR0 | admin + one I/O queue pair, namespace 1 only, 512-byte blocks; controller must be below 4 GB |
| `fd1` | `fdc.h` | the boot **floppy** (8272/82077 + ISA DMA 2) | only when booted from drive 00h/01h | 1.44 MB; whole-track read cache; motor left on |
| (none) | usb | -- | -- | not implemented |

A disk's size is capped at 2^28 - 1 sectors (128 GB) everywhere, like IDE LBA28.

## Network (`nic.h` is the common interface)

| file | hardware | notes |
|---|---|---|
| `e1000.h` | Intel 8254x: 82540EM, 82545EM, 82544GC, 82543GC (PCI ids 0x100E, 0x100F, 0x100C, 0x1004) | MMIO; legacy descriptors; the 82543GC id is untested (no QEMU model) |
| `rtl8139.h` | Realtek RTL8139 | I/O ports; 4-slot round-robin transmit |

`net.h`/`net_stack.h` and the protocol headers (`arp.h`, `ip.h`, `icmp.h`, `udp.h`, `dhcp.h`, `dns.h`, `tcp.h`, `tls.h`, `https.h`)
sit on top. Not supported: VMware e1000e/vmxnet3, VirtualBox PCnet, virtio-net.

## Platform and human interface

| file | what | notes |
|---|---|---|
| `pci.h` | PCI configuration space scan | legacy ports 0xCF8/0xCFC; fills `pci_devices[]`, dumps every device to the serial port |
| `vga.h` | the 640x480 32-bit VESA framebuffer | the mode is set by the boot loader; the kernel only draws (to a back buffer, then copies) |
| `keyboard.h`, `mouse.h` | PS/2 (i8042) keyboard and mouse | polled; USB HID is the next hardware goal |
| `rtc.h` | CMOS real-time clock | wall-clock time; also what network timeouts count in (`netclock.h`) |
| `delay.h` | PIT-based millisecond waits | used by the floppy and ATAPI drivers |
| `speaker.h` | PC speaker beep | |
| `serial.h` | COM1 debug log | everything the kernel finds at start-up goes here |
| `acpi.h` | ACPI S5 power-off (RSDP > RSDT > FADT, `_S5_` from the DSDT) | falls back to the QEMU/Bochs/VirtualBox power-off ports under a hypervisor |
| `fatal.h` | the interrupt descriptor table (32 exception vectors) and the blue-screen error | `MW-K10 CPU_EXCEPTION_*` |
| `sysinfo.h` | CPUID, TSC speed and load, PCI display adapter name, VESA video memory, RAM size | for `fetch` |

## Conventions all drivers follow

* **Bounded waits.** Every wait loop has an iteration or time limit; a missing or hung device fails the operation, never the machine.
* **Failure is a return value.** `1` = worked, `0` = failed; the caller (the file system, `os-inst`) decides what that means.
* **DMA memory is named** in `memmap.h` and sits below 4 GB; with no paging a pointer is the bus address.
* **Compiler barriers** (`MW_BARRIER()`) bracket the hand-off to a device: fill the structure, barrier, ring the doorbell.
* **Probe once, idempotently.** `*_init()` runs on the first `st_scan()` and a second call does nothing.
* **Say what you found** on the serial port in a `[TAG]` line, so a log from someone else's machine shows what the kernel saw.
