# MiniWin Icon Bundle

Source: hand-made for this project by the user (SegFault777), supplied
as a 16x16/32x32 PNG bundle (`MiniWin_IconsBundle_v1_0-preONLY.zip`).
Not a third-party/external asset in the usual third_party/ sense (no
separate upstream license applies: like the rest of the project's own
material it is covered by the project license, GPL-3.0-only -- see
the top-level LICENSE) -- vendored here purely so the
bundle's original PNG sources sit alongside the disk-image installer
that reads them, the same way third_party/galmuri-font/ keeps a font's
source material next to the tool that consumes it.

`Images/16x16/` and `Images/32x32/` hold the individual icon PNGs
(Notepad, Setting, Web, TrashCan, Folder, PC, FileManager, and a set of
per-language "_Code" file-type icons for future use, e.g. by a File
Manager app). `Color.txt` is the bundle's reference palette.

These are installed into the OS disk image's icon catalog (see
kernel/fs.h's ICON_* constants) by tools/install_icons.py -- run that
script against a built os-image.img to write the catalog; nothing
reads these PNG files directly at kernel build time or at runtime.

The bundle now also carries the **storage icons** (`IDE_Drive`, `SATA_Drive`, `NVMe_Drive`, `SCSI_Drive`, `SAS_Drive`,
`iSCSI_Drive`, `Drive`, `Unknown_Drive`, `CD-ROM`, `DVD`, `BLUERAY`, `SD_Card`, `miniSD_Card`, `microSD_Card`,
`Cloud_Drive`) in 32x32 and 16x16, plus `NAS`, `FD_12`, `FD_144`, `FD_50`, `Download_and_install` and `Upload` in 32x32 only.
47 distinct icons in all; the disk-image catalog has 56 slots (kernel/fs.h `ICON_MAX_SLOTS`).

Where they are used (1.1-alpha-1): the desktop icons (Notepad, Setting, Web, Terminal, MiniFiles' `FileManager`, the
document icon) are drawn from the catalog as bitmaps; MiniFiles shows 16x16 icons beside its rows (`Doc`, `ASM_Code`,
`IDE_Drive`, `Drive`). The other icons are installed and waiting: the per-bus drive icons are for the day `sata`, `nvme`,
`scsi` and `usb` storage have drivers.
