# MiniWin SDK

Everything you need to write a program for MiniWin (a `.MWP` file) lives in this folder and the OS repository around it.
A program is one C file, compiled freestanding for the 32-bit x86 kernel, linked to run at a fixed address, and copied into
`/progs` of a disk image. There is no libc and no startup code: the kernel hands the program a table of functions and jumps in.

```
sdk/
  include/miniwin.h     the header a program includes: the table's address (sys), colours, small inline helpers
  include/mwp_abi.h     the syscall table itself -- ONE definition, read by the kernel and by programs alike
  examples/             hello.c (clock, memory, icons), browse.c (folders + files), paint.c (mouse + saving a file)
  build.sh              sdk/build.sh sdk/examples/hello.c            ->  build/hello.mwp
  install.sh            sdk/install.sh sdk/examples/hello.c HELLO.MWP  ->  writes it into build/os-image.img
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

## Rules a program lives by

1. **No `.bss`.** The loader copies the file and jumps in; it clears nothing. Do not rely on uninitialised globals being zero --
   use locals, or `sys.mem_alloc()` (zero-filled).
2. **24 KB at most** for code and data together (the build checks, and says how close you are).
3. **You own the screen while you run.** The desktop is not redrawn until you return, so draw everything every frame (or keep
   what you drew) and call `sys.present()`. Return from `mwp_entry()` to quit.
4. **Poll, don't block.** `sys.key_poll()` returns 0 when no key is waiting. Do not `sleep_ms()` for long inside a loop that also
   watches the keyboard: the controller keeps one byte, and a key pressed and released during your sleep is lost.
5. **There is no memory protection.** A program is ring-0 code: a wild pointer can crash the machine or damage the disk. Check
   what you pass to the kernel, and test in QEMU before real hardware. (Ring 3 and windowed programs are planned.)

## The syscall table (ABI version 2)

Check `mw_has(2)` before using anything below the version-1 group; a 1.1-alpha-9 kernel only has version 1.

| group | functions |
|---|---|
| drawing (v1) | `put_pixel`, `fill_rect`, `draw_rect`, `draw_string` (Latin), `draw_string_ko` (Latin + Hangul), `present`, `screen_w`, `screen_h` |
| input | `key_poll` (v1), `mouse_poll(&x,&y,&buttons)` (v2: bit 0 = left, bit 1 = right; returns 1 when something changed) |
| memory (v2) | `mem_alloc(n)` zero-filled, 16-aligned, 0 when out of memory; `mem_free(p)`; `mem_free_kb()`. At most 64 blocks and 4 MB per run; whatever you forget is freed when you return |
| files (v2) | `file_size`, `file_read(path, buf, max)`, `file_write(path, data, len)`, `file_delete`, `dir_make`, `dir_list(path, callback, ctx)`; paths are absolute (`/docs/DOC.TXT`); errors are negative numbers, `err_text(e)` says what happened in words |
| time (v2) | `ticks_ms()` (call at least every 2 s), `sleep_ms(n)`, `time_now(&t)` (the hardware clock) |
| UI helpers (v2) | `draw_icon16`/`draw_icon12` (`MWI_*`), `text_width`, `bevel`, `button`, `window_frame(x,y,w,h,title,icon)` |
| misc (v2) | `log(s)` to the serial port, `rand32()` |

A file written under `/docs` or `/progs` shows up on the desktop straight away.

## Keeping the two sides in step

`mwp_abi.h` is included by `kernel/mwp.h` and by `miniwin.h`, so the kernel and every program are compiled against the same
layout. Fields are only ever **appended**; an existing field never moves or changes meaning. That is what lets a program built
today keep running on a later kernel, and it is what the planned Ring 3 / windowed-program work builds on.
