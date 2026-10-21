# Memory map

MiniWin runs in one flat 32-bit address space with **no paging and no memory protection** (everything, including `.MWP` programs, runs
in ring 0). There is no allocator: every big buffer lives at a fixed, named address, and `kernel/memmap.h` is the **single source of
truth** (compile-time asserts there keep regions from overlapping). The table is the picture as of 1.1-alpha-8; when it disagrees
with `memmap.h`, the header wins.

| range | what | defined in |
|---|---|---|
| `0x000000`-`0x0004FF` | real-mode leftovers: interrupt vector table, BIOS data area | BIOS |
| `0x000500`-`0x00050E` | **boot info block** the loader leaves: BIOS drive (`0x500`), install base (`0x504`), install id (`0x508`), floppy geometry (`0x50C`/`0x50D`) | `boot/boot.asm`, `boot/stage2.asm` |
| `0x000600`-`0x000DFF` | stage 2 (2 KB) | `boot/stage2.asm` |
| `0x001000`-`0x004FFF` | stage 2's CHS bounce buffer (only while loading from a floppy) | `boot/stage2.asm` |
| `0x006000` | the VESA controller info block (read later by `fetch`) | `boot/stage2.asm` |
| `0x007C00`-`0x007DFF` | the boot sector as the BIOS loaded it (the kernel compares it with the disk to find its boot disk) | BIOS |
| `0x010000`-`0x0xxxxx` | the **kernel image** (`.text`, `.rodata`, `.data`) followed by `.bss`; `build.sh` checks image <= 512 KB and `.bss` below `0x9FC00` | `kernel/kentry.asm`, `build.sh` |
| `0x100000`-`0x17FFFF` | the kernel **stack** (512 KB, grows down from `0x180000`) | `MW_STACK_TOP` |
| `0x180000`-`0x3FFFFF` | the **network arena** (2.5 MB): TCP and TLS buffers, the 256 KB HTTP body, NIC DMA rings (e1000, RTL8139) | `MW_NETMEM_*` |
| `0x400000`-`0x52BFFF` | the **back buffer** (640 x 480 x 32-bit) the desktop is composed in | `VGA_BACKBUF_PHYS_ADDR` |
| `0x530000`-`0x7BFFFF` | the **HTML5 engine's** arenas (DOM nodes, CSS rules, layout items, scratch) | `MW_RD_*` |
| `0x7C0000`-`0x7C6FFF` | 32x32 **icon cache** (7 icons: NOTEPAD, SETTING, WEB, DOC, TERMINAL, FILEMANAGER, PROGRAM) | `MW_ICON_CACHE_*` |
| `0x7C7000`-`0x7C7FFF` | MiniFiles' four 16x16 icons | `MW_FILES_ICON_*` |
| `0x7D0000`-`0x80CFFF` | the **Korean font**, inflated from the compressed table at boot (245,784 bytes) | `MW_KFONT_*` |
| `0x810000`-`0x82FFFF` | **storage DMA**: AHCI (`+0x0000`, 8 x 2 KB), SCSI/MPT (`+0x4000`, 2 x 4 KB), NVMe (`+0x6000`, 2 x 24 KB), floppy (`+0x12000`, track buffer + sector) | `MW_STOR_DMA_*` |
| `0x8F0000`-`0x8F0FFF` | the **MWP syscall table** (programs find the kernel's functions here) | `MW_MWP_SYSCALL_ADDR` |
| `0x8F1000`-`0x8FFFFF` | where a **`.MWP` program** is loaded and run (up to 24 KB used) | `MW_MWP_LOAD_ADDR` |
| `0x900000`- | free (beyond the end of the fixed regions) | |

The kernel needs about 9.2 MB of RAM in total (`VGA_MIN_RAM_KB`); `fetch` reports the reserved part (everything below the end of the
last fixed region) against the RAM the BIOS reports (`INT 15h` `E801h`, saved at `0x9200`).

## Why it is built this way

* **Fixed addresses instead of an allocator.** No fragmentation, no failure paths for "out of memory" at run time, and every
  buffer's size is a reviewed constant. The price: everything is sized for the worst case, and nothing can grow on demand.
* **DMA buffers.** Devices read and write physical addresses; with no paging a pointer *is* the bus address, so the DMA regions are
  just named slices of RAM below 4 GB (below 16 MB for the floppy's ISA DMA, and never across a 64 KB boundary).
* **Big tables ride compressed.** The image has a hard 512 KB budget (the boot loader reads exactly 1024 sectors), so the Korean font
  is LZ4-compressed in the image and inflated into the region above ([lz.h](../../kernel/lz.h)).

## Changing the map

Add the region and its size in `memmap.h`, include an assert next to the others that it fits and does not overlap, use the named
constants everywhere (host tests substitute ordinary arrays by defining the macro first), and update this table.
