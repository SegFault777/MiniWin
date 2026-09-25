# MiniWin Icon Bundle

Source: hand-made for this project by the user (SegFault777), supplied
as a 16x16/32x32 PNG bundle (`MiniWin_IconsBundle_v1_0-preONLY.zip`).
Not a third-party/external asset in the usual third_party/ sense (no
separate upstream license applies) -- vendored here purely so the
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

As of this bundle's addition, the actual desktop/window-chrome icon
*rendering* in kernel/kernel.c still draws the older hand-coded vector
glyphs (a few bb_rect()/bb_putpixel() calls per icon) -- swapping that
rendering over to blit these bitmaps instead is separate, later work
(see the Windows-9x-style UI pass this bundle was supplied for).
