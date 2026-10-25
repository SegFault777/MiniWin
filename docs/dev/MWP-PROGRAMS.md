# Programs (`.MWP`)

A **`.MWP`** ("MiniWin Program") is a small flat binary that the kernel copies to a fixed address and jumps into. This page documents
the interface **as it is today** (1.1-alpha-9); a larger SDK with a proper API is planned and will extend it.

> **Important: there is no isolation.** MiniWin has no paging and no ring 3 yet. A program runs in ring 0 with the kernel: a buggy
> program can crash the machine, overwrite kernel memory or hang it, exactly as easily as a kernel bug could. Ring 3 is planned
> (see [ROADMAP.md](../project/ROADMAP.md)).

## What a program is

* Raw x86 machine code, compiled freestanding (no libc, no startup files, no relocation) and **linked to run at `0x8F1000`**
  (`MWP_LOAD_ADDR`). The first byte (plus an optional entry offset, normally 0) is the first instruction; it is called with no
  arguments.
* At most **24,576 bytes** (`PROG_MAX_BYTES`), stored as an ordinary file in `/progs` of the root file system (the
  entry offset is kept in the file's inode, see [DISK-LAYOUT.md](DISK-LAYOUT.md)); the file itself is pure code and data. Any number
  of programs fit (the desktop and MiniFiles show the first 32). `cp` can copy a program in from a mounted disk.
* One program runs at a time, and it must return (or loop calling `key_poll`) -- there is no scheduler.

## The syscall table

Programs are built separately from the kernel, so they cannot know the kernel's function addresses. Instead the kernel fills in a
table of function pointers at the fixed address **`0x8F0000`** (`MWP_SYSCALL_TABLE_ADDR`) before it jumps. The layout is the C struct
`mwp_syscalls_t` in `kernel/mwp.h`, mirrored for programs in `programs/mwp_api.h`:

| member | meaning |
|---|---|
| `put_pixel(x, y, color)` | set one pixel in the back buffer |
| `fill_rect(x, y, w, h, color)` | filled rectangle |
| `draw_rect(x, y, w, h, color)` | rectangle outline |
| `draw_string(x, y, s, color)` | text, Latin only |
| `draw_string_ko(x, y, s, color)` | text, Latin and Hangul mixed (UTF-8) |
| `present()` | show the back buffer on the screen -- **nothing is visible until this is called** |
| `key_poll()` | one pending key as an ASCII byte, `0` if none; never blocks |
| `screen_w`, `screen_h` | screen size (640 x 480) |

Colours are 32-bit `0x00RRGGBB`. The table is read-only for programs and its order is the ABI: **never reorder it**, only append.

## Build and install a program

```sh
tools/build_mwp.sh programs/greeter.c        # -> build/greeter.mwp (flat binary, linked at 0x8F1000)
python3 tools/install_mwp.py build/os-image.img 0 GREETER.MWP build/greeter.mwp   # the 0 is a leftover slot number, ignored; installing again replaces
```

`tools/install_all.sh` does this for the sample program (`programs/greeter.c`, as `/progs/GREETER.MWP`). The program then shows up as a desktop
shortcut, in MiniFiles and in MiniShell's `apps`, and starts with a double-click or `run greeter`.

## Writing one

Start from `programs/greeter.c`: include `programs/mwp_api.h`, get the table pointer, draw, call `present()`, poll keys. Keep to what
a freestanding program can do: no libc (no `printf`, no `malloc`), no global constructors, no floating-point library calls; stack
space is the kernel's stack. Linker script: `kernel/mwp_link.ld` (the load address is kept in three places that must agree --
`memmap.h`, the linker script and `mwp_api.h`; `mwp.h` asserts the first).

## What is coming

The plan is to turn this into a proper SDK (a `sdk/` directory with headers, a linker script, a build helper and examples), a richer
API (windows, files, events, networking) and, last, ring 3 with a system-call gate and per-program address spaces. Until then treat
the table above as the stable contract.
