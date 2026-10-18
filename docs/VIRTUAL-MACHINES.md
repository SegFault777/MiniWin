# Running MiniWin in VMware, VirtualBox and other virtual machines

MiniWin is developed and tested on **QEMU**. Everything below about VMware and VirtualBox comes from their documented
hardware and from running the *same device models* QEMU emulates; it has **not** been run on VMware Workstation/Player or
VirtualBox by the developer. If something here does not match what you see, the boot error name and numbers
([BOOT-PROCESS.md](BOOT-PROCESS.md), "Error codes") are what is needed to fix it.

## The three things that decide whether it boots

1. **Legacy BIOS, not UEFI.** MiniWin is loaded by BIOS INT 13h calls from a boot sector. A virtual machine set to UEFI
   (VirtualBox: "Enable EFI" ticked; VMware: Options > Advanced > Firmware type = UEFI, the default for some newer guest types
   such as Windows 11) will not find a bootable disk. Choose **BIOS / Legacy**.
2. **Attach the image as a hard disk (recommended) or as a floppy.** As a hard disk use `os-image.img` (a raw 1 MB disk; convert it
   as below). As a **floppy** (VMware: Floppy > "Use a floppy image file"; VirtualBox: a floppy controller with the image
   inserted) use `build/os-image-floppy.img`, the same image padded to 1.44 MB, and boot from the floppy: the loader reads it by
   CHS and the kernel runs from it through its floppy driver (alpha-7 and later -- before that this was the
   `MW-B01 BIOS_LBA_UNSUPPORTED (BIOS AH=01 DL=00)` error). Not a CD/DVD. For a hard disk, convert the image:
   * VMware: `qemu-img convert -f raw -O vmdk os-image.img MiniWin.vmdk` and add it as an existing virtual disk.
   * VirtualBox: `VBoxManage convertfromraw os-image.img MiniWin.vdi --format VDI`, then attach it to a controller.
3. **At least ~16 MB of RAM** (the kernel refuses with `MW-K01 NOT_ENOUGH_RAM` below about 9.2 MB; 64 MB is comfortable) and a
   display that offers a **640x480 32-bit VESA mode** (all of them do).

## Which virtual hardware works

| what | works | does not |
|---|---|---|
| disk controller | **floppy (1.44 MB, the boot floppy)**, IDE, **SATA (AHCI)** (VirtualBox's default, VMware "SATA"), **LSI Logic / LSI Logic SAS** (VMware's SCSI default, VirtualBox "LsiLogic"), **NVMe** (VMware 17+, VirtualBox 6.1+) | BusLogic, VMware Paravirtual SCSI, virtio-blk/virtio-scsi, USB storage |
| network card | **Intel e1000** family: VMware "e1000", VirtualBox "Intel PRO/1000 MT Desktop / MT Server / T Server"; RTL8139 | VMware e1000e and vmxnet3, VirtualBox PCnet-FAST III / virtio-net |
| shut down | ACPI power-off: the VM really switches off | |

MiniWin finds the disk it was booted from by itself, on any of those buses (see BOOT-PROCESS.md), so the controller type does
not need to be configured anywhere. A second disk of any supported type can be a target for `os-inst`.

## If it does not boot

Read the error on the screen. Every boot error has a code and a name, and shows the BIOS's own error code, the sector (LBA)
and the drive number where they apply:

| you see | meaning, first thing to try |
|---|---|
| `MW-B01 BIOS_LBA_UNSUPPORTED` | retired in alpha-7 (floppies boot now); seen on older builds when the image was attached as a floppy |
| `MW-B02 STAGE2_READ_FAILED (BIOS AH=04...)` | the disk cannot be read at sector 1: is the image intact, and is it the first boot disk? |
| `MW-B03 KERNEL_READ_FAILED ... LBA=...` | the disk is shorter than the kernel needs (`AH=04` and an LBA near the end: a truncated or wrongly converted image) |
| `MW-B04` / `MW-B05` | the virtual graphics card has no VESA mode list / no 640x480 32-bit mode |
| a blue screen `MW-K10 CPU_EXCEPTION_...` | the kernel crashed; report the name, vector, error code and EIP |
| black screen / reboot loop right after the BIOS | firmware is UEFI (see above), or the disk is not the first boot device |

On top of this, the serial port (VMware: add a serial port to a file; VirtualBox: Serial Ports > COM1 > Raw File) receives a
`[BOOT]`, `[PCI]`, `[AHCI]`/`[MPT]`/`[NVMe]` log that shows which controllers and disks the kernel found.
