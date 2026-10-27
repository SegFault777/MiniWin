# MiniWin SDK 2.0

(`sdk/VERSION`; `MW_SDK_VERSION` in `miniwin.h`. The SDK's version is not the ABI's: the ABI is `sys.abi_version`, 3 since 1.1-alpha-11.)

Everything you need to write a program for MiniWin (a `.MWP` file) lives in this folder and the OS repository around it.
A program is one C file, compiled freestanding for the 32-bit x86 kernel, linked to run at a fixed address, and copied into
`/progs` of a disk image. There is no libc and no startup code: the kernel hands the program a table of functions and jumps in.

```
sdk/
  include/miniwin.h     the header a program includes: the table's address (sys), colours, small inline helpers; pulls in the three below
  include/mw_libc.h     strings, mw_snprintf, mw_isqrt, mw_muldiv (64-bit intermediate), sine/cosine, sort -- no libc needed
  include/mw_gfx.h      lines, circles, gradients, aligned/wrapped text, mw_printf, bitmaps
  include/mw_ui.h       an immediate-mode toolkit: buttons, check boxes, text boxes, lists, progress bars, message boxes, file/clipboard helpers
  include/mwp_abi.h     the syscall table itself -- ONE definition, read by the kernel and by programs alike
  examples/             hello.c (clock, memory, icons), browse.c (folders + files), paint.c (mouse + saving a file),
                        minicalc.c (calculator: toolkit, clipboard, own icon, set_size, args), crash.c (provokes every fault paging catches)
  build.sh              sdk/build.sh sdk/examples/hello.c            ->  build/hello.mwp
  install.sh            sdk/install.sh sdk/examples/hello.c HELLO.MWP  ->  writes it into build/os-image.img

The whole OS image without the demos and examples: `./build.sh && tools/install_all.sh --no-demos`.
```

## Hello

```c
#include "miniwin.h"

void mwp_entry(void) {                  /* the program starts here: byte 0 of the file */
    mw_fill_screen(MW_DESKTOP);
    mw_text(20, 20, "Hello, MiniWin!", MW_BLACK);
    sys.present();                      /* nothing is visible until you present */
    mw_wait_key();
}                                       /* returning hands the screen back to the desktop */
```

Build and try it: `./build.sh && sdk/install.sh my.c MY.MWP`, boot the image, open MiniFiles, go to `/progs`, double-click it.
Requirements: `gcc` with 32-bit support (`gcc-multilib`), `nasm`, `python3`; `qemu-system-i386` to run it.

## Your own icon and title

Put `NAME.png` (any size; it is scaled to 16x16 and 32x32) next to `NAME.c` and add two comment lines to the source:

```c
/* @mwp title: MiniCalc */      /* shown in the program's window title bar */
/* @mwp abi: 3 */               /* the oldest syscall-table version it needs: an older kernel refuses to start it */
```

`sdk/build.sh` packs them into the file (or pass `--icon x.png --title "Name" --abi 3` to `tools/build_mwp.sh`). The icon appears on the desktop, in
the program's window title bar and task bar button, and in MiniFiles' `/progs` list; a windowed program can change its window icon while it
runs with `sys.set_icon(argb16)`. `run NAME some text` hands `some text` to the program as `sys.args`.

## The toolkit in ten lines

```c
mw_ui_t ui; mw_ui_init(&ui);
for (;;) {
    mw_ui_begin(&ui);                                        /* mouse + keyboard once per frame */
    mw_fill_screen(mw_ui_color(MW_TH_FACE));                 /* the theme's grey: follows the dark theme */
    if (mw_ui_button(&ui, 10, 10, 80, 22, "Click")) count++;
    mw_printf(10, 40, mw_ui_color(MW_TH_TEXT), "clicked %d times", count);
    mw_ui_end(&ui);                                          /* present (and, in a window, give the CPU back) */
    if (ui.key == MW_KEY_ESC) break;
}
```

## Rules a program lives by

1. **No `.bss`.** The loader copies the file and jumps in; it clears nothing. Do not rely on uninitialised globals being zero --
   use locals, or `sys.mem_alloc()` (zero-filled).
2. **32 KB at most** for the whole file -- code, data and the icon/title trailer (the build checks, and says how close you are).
3. **You own the screen while you run.** The desktop is not redrawn until you return, so draw everything every frame (or keep
   what you drew) and call `sys.present()`. Return from `mwp_entry()` to quit.
4. **Poll, don't block.** `sys.key_poll()` returns 0 when no key is waiting. Do not `sleep_ms()` for long inside a loop that also
   watches the keyboard: the controller keeps one byte, and a key pressed and released during your sleep is lost.
5. **Protection is partial.** With paging (1.1-alpha-11) a null pointer, a write into the kernel's code, a stack overflow, a divide by zero or a
   bad instruction stops *your* program in a window and nothing else (`examples/crash.c` provokes each). But a program still runs in ring 0, so
   a wild write into the kernel's data can crash the machine or damage the disk. Check what you pass to the kernel; test in QEMU first.
   (Ring 3 is planned.)

## Windows (since alpha-11)

`run NAME.MWP` starts your program **in a window**; `run -f NAME.MWP` gives it the whole screen. The same code works in both:
`sys.screen_w` / `sys.screen_h` are the size of the area you may draw in, `mw_windowed()` tells you which case you are in, and
`sys.set_title("...")` sets the title bar text of the window. What changes for the program:

* The window is **not** yours alone: the desktop keeps running, so `sys.present()`, `sys.key_poll()` (when no key is waiting) and
  `sys.sleep_ms()` are where you hand the CPU back. A loop that calls none of them freezes the whole OS. Call `present()` once per frame.
* The area can be **resized** between frames: read `sys.screen_w/h` every frame rather than once.
* Keys arrive only while your window is in front. Mouse coordinates are relative to your area, `buttons` is 0 outside a drag you
  started inside. Do not draw your own title bar or window frame when `mw_windowed()`.
* Only one program window at a time (all programs share one load address). Closing the window stops you wherever you were; blocks from
  `mem_alloc` are freed for you.

## The syscall table (ABI version 3)

Check `mw_has(2)` (or 3) before using anything beyond the version-1 group; a 1.1-alpha-9 kernel only has version 1.

| group | functions |
|---|---|
| drawing (v1) | `put_pixel`, `fill_rect`, `draw_rect`, `draw_string` (Latin), `draw_string_ko` (Latin + Hangul), `present`, `screen_w`, `screen_h` |
| input | `key_poll` (v1), `mouse_poll(&x,&y,&buttons)` (v2: bit 0 = left, bit 1 = right; returns 1 when something changed) |
| memory (v2) | `mem_alloc(n)` zero-filled, 16-aligned, 0 when out of memory; `mem_free(p)`; `mem_free_kb()`. At most 64 blocks and 4 MB per run; whatever you forget is freed when you return |
| files (v2) | `file_size`, `file_read(path, buf, max)`, `file_write(path, data, len)`, `file_delete`, `dir_make`, `dir_list(path, callback, ctx)`; paths are absolute (`/docs/DOC.TXT`); errors are negative numbers, `err_text(e)` says what happened in words |
| time (v2) | `ticks_ms()` (call at least every 2 s), `sleep_ms(n)`, `time_now(&t)` (the hardware clock) |
| UI helpers (v2) | `draw_icon16`/`draw_icon12` (`MWI_*`), `text_width`, `bevel`, `button`, `window_frame(x,y,w,h,title,icon)` |
| misc (v2) | `log(s)` to the serial port, `rand32()` |
| window (v3) | `run_flags` (`MW_RUN_WINDOWED`), `set_title(s)`, `set_size(w, h)` (resize the window to that drawing area), `set_icon(argb16)`, `args` |
| more (v3) | `clipboard_get/set` (shared with Notepad), `theme_color(MW_TH_*)`, `beep(hz, ms)` |

A file written under `/docs` or `/progs` shows up on the desktop straight away.

## Keeping the two sides in step

`mwp_abi.h` is included by `kernel/mwp.h` and by `miniwin.h`, so the kernel and every program are compiled against the same
layout. Fields are only ever **appended**; an existing field never moves or changes meaning. That is what lets a program built
today keep running on a later kernel, and it is what the planned Ring 3 / windowed-program work builds on.
