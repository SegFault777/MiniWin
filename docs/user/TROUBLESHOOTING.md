# Troubleshooting

## Error messages at start-up

If MiniWin cannot start, it says **what** failed with a code and a name instead of a vague "disk error". The code starts with
`MW-`: **B** = the boot loader, **C** = the partition chain loader, **K** = the kernel. Where it helps, the message also shows the
BIOS's own error code (`AH=...`), the sector number (`LBA=...`) and the BIOS drive number (`DL=...`; `00` is a floppy, `80` the
first hard disk). A blue screen (`MW-K...`) is the kernel stopping on purpose and telling you why.

| you see | in plain words | what to try |
|---|---|---|
| `MW-B02 STAGE2_READ_FAILED` | the very first sectors of the disk could not be read | check that the image is attached as the first boot disk (or as the floppy) and is not damaged; re-run `./build.sh` |
| `MW-B03 KERNEL_READ_FAILED ... LBA=...` | the kernel could not be read from that sector on | the disk is shorter than the image (a cut-off copy or a wrong conversion; `AH=04` near the end means "past the end of the disk") |
| `MW-B04 VBE_INFO_UNAVAILABLE` / `MW-B05 VBE_NO_640x480x32_MODE` | the (virtual) graphics card has no usable VESA mode | use a standard VGA / VESA-capable adapter; every common virtual machine has one |
| `MW-C01`..`MW-C03` | starting MiniWin from inside a partition failed (no active partition / unreadable / not bootable) | re-run the installation, or mark the right partition active |
| `MW-K01 NOT_ENOUGH_RAM` | too little memory | give the machine 64 MB |
| `MW-K02 VBE_PIXEL_FORMAT_UNSUPPORTED` | the graphics mode's colour layout is unusual | try another graphics adapter setting |
| blue screen `MW-K10 CPU_EXCEPTION_...` | the kernel hit an unexpected CPU exception | note the exception name, `EIP` and registers shown (a photo is fine) and report them: <https://github.com/SegFault777/MiniWin/issues> |
| `MW-B01 BIOS_LBA_UNSUPPORTED` | (old builds only) the image was attached as a floppy; current builds start from a floppy too | update to the newest build |

The older message *"MiniWin: DISK READ ERROR"* no longer exists; it is replaced by the codes above.

## It does not start at all (black screen, reboot loop)

* The firmware is probably **UEFI**. MiniWin needs the legacy **BIOS** (VMware: *Options > Advanced > Firmware type: BIOS*;
  VirtualBox: leave *Enable EFI* unticked).
* The disk is not the **first boot device**.
* Floppy image: use `os-image-floppy.img` (1.44 MB) as the floppy and boot from the floppy.

## No network

* Use a supported card: Intel e1000 family or Realtek RTL8139 ([FILES-AND-DISKS.md](FILES-AND-DISKS.md)). VMware's *vmxnet3* and
  *e1000e* and VirtualBox's *PCnet* are not supported yet.
* The network must hand out addresses by DHCP (QEMU `-nic user`, VMware NAT and VirtualBox NAT do).
* `fetch` in MiniShell shows the IP address, or *offline*.

## Everything looks fine but a disk is missing in MiniFiles / `os-inst list`

* USB disks are not supported. USB keyboards, mice and tablets are (xHCI/EHCI/OHCI); devices behind a USB hub are not seen, and neither are Intel UHCI controllers.
* A SATA disk behind a RAID-mode controller is not seen (only AHCI is supported).
* SCSI disks must be on an LSI Logic controller; other SCSI adapters (BusLogic, VMware Paravirtual, virtio) are not supported.

## My documents are gone

Documents live **inside the disk image**. Building again (`./build.sh`) creates a fresh image, and `tools/install_all.sh` writes the
sample program and icons into whichever image you point it at. Keep a copy of your image file before rebuilding.

## It feels slow

MiniWin draws everything with the CPU and never sleeps; in an emulator without hardware acceleration (QEMU without KVM) heavy pages
and large windows are slower. `fetch` shows the measured CPU speed and MiniWin's own busy share.

## Reporting a problem

Please include: the version (`ver` is not a command, but `fetch` prints it on its first line), what you ran it on (VMware,
VirtualBox, QEMU, real PC), the exact error name and numbers if one appeared, and -- if you can -- the serial port log (VMware: add a
serial port that writes to a file; VirtualBox: *Serial Ports > COM1 > Raw File*; QEMU: `-serial file:serial.log`). Everything MiniWin
finds at start-up is written there as lines starting with `[BOOT]`, `[PCI]`, `[AHCI]`, `[NVMe]`, `[FDC]`, ....
