# MiniWin Icon Bundle

Source: hand-made for this project by SegFault777, supplied as a 16x16/32x32 PNG bundle.

## License: CC BY-SA 4.0

**The icons in this bundle are licensed under the Creative Commons Attribution-ShareAlike 4.0 International license
(CC BY-SA 4.0)** -- <https://creativecommons.org/licenses/by-sa/4.0/> (legal code:
<https://creativecommons.org/licenses/by-sa/4.0/legalcode>), by their author, SegFault777.
SPDX identifier: `CC-BY-SA-4.0`. This is deliberately NOT the project's code license (GPL-3.0-only, see the top-level
`LICENSE`): the code and the artwork are separate works under separate licenses.

What that asks of you, in short (the license text governs, this is a summary): you may share and adapt the icons for any
purpose, including commercially, as long as you (1) give appropriate credit -- for example "MiniWin icon pack by SegFault777,
CC BY-SA 4.0" with a link to the license and a note of any changes you made -- and (2) distribute your adaptations under the
same license (CC BY-SA 4.0, or a license CC lists as compatible).

Where the icons end up: `tools/install_icons.py` converts the PNGs into bitmaps in the icon catalog of a disk image
(`kernel/fs.h`, `ICON_*`). Those catalog sectors are the same artwork in another format, so they stay under CC BY-SA 4.0; the
kernel and tools on the same disk image remain GPL-3.0-only. They are distributed together as a collection, not merged into one
work, and neither license is changed by that. The one exception to "the pack is not part of the kernel binary" is the five
**mouse cursors** (`Images/Cursor/`): `tools/gen_cursors.py` turns them into `kernel/cursor_data.h`, which is compiled into the
kernel image. That header is the same artwork in another format and carries `SPDX-License-Identifier: CC-BY-SA-4.0`; the kernel
binary therefore contains CC BY-SA 4.0 bitmaps next to GPL-3.0-only code (credit and share-alike apply to those bitmaps).

This directory is vendored under `third_party/` so the bundle's original PNG sources sit alongside the disk-image installer
that reads them, the same way `third_party/galmuri-font/` keeps a font's source material next to the tool that consumes it.

`Images/16x16/` and `Images/32x32/` hold the individual icon PNGs
(Notepad, Setting, Web, TrashCan, Folder, PC, FileManager, and a set of
per-language "_Code" file-type icons for future use, e.g. by a File
Manager app); `Images/Cursor/` holds the five mouse cursors (arrow, horizontal,
vertical and two diagonal resize pointers: black outline, white fill). `Color.txt` is the bundle's reference palette.

These are installed into the OS disk image's icon catalog (see
kernel/fs.h's ICON_* constants) by tools/install_icons.py -- run that
script against a built os-image.img to write the catalog; nothing
reads these PNG files directly at kernel build time or at runtime.

The bundle also carries the **storage icons** (`IDE_Drive`, `SATA_Drive`, `NVMe_Drive`, `SCSI_Drive`, `SAS_Drive`,
`iSCSI_Drive`, `Drive`, `Unknown_Drive`, `CD-ROM`, `DVD`, `BLUERAY`, `SD_Card`, `miniSD_Card`, `microSD_Card`,
`Cloud_Drive`) in 32x32 and 16x16, plus `NAS`, `FD_12`, `FD_144`, `FD_50`, `Download_and_install` and `Upload`.

**Icon pack update (1.1-alpha-2 era).** The supplied update renames three icons (`C#_Code` -> `CS_Code`, `C++_Code` ->
`CPP_Code`, `Doc` -> `doc`; the installer matches names case-insensitively and uppercases them, so `DOC` is unchanged) and
adds `CSS_Code`, `Open`, `Open2`, `Program`, `Refresh`, the `USB_2_0`, `USB_3_0_3_1_3_2_Gen_1` and `USB_3_2_Gen_2` drive
icons, and 16x16 versions of `NAS`, the floppy icons, `Download_and_install` and `Upload`. Every icon that existed before is
byte-identical. 55 distinct icons in all (the 16x16-only ones are `ASM_Code` and `PC`; `ARMASM_Code`, `Earth`, `Terminal`
and `X86ASM_Code` are 32x32-only); the disk-image catalog has 56 slots (kernel/fs.h `ICON_MAX_SLOTS`), so **one slot is
spare** -- raising the catalog is a disk-layout change (docs/dev/DISK-LAYOUT.md) before the bundle grows again.

Where they are used (1.1-alpha-2): the desktop icons (Notepad, Setting, Web, Terminal, MiniFiles' `FileManager`, the
document icon) are drawn from the catalog as bitmaps; MiniFiles shows 16x16 icons beside its rows (`Doc`, `ASM_Code`,
`IDE_Drive`, `Drive`). The other icons are installed and waiting: `SATA_Drive` is for the `sata` disks that have had a
driver since alpha-2, and the remaining per-bus drive icons are for the day `nvme`, `scsi` and `usb` storage have drivers.
