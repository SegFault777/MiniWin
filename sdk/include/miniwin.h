/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MINIWIN_H
#define MINIWIN_H

/* miniwin.h -- the one header a MiniWin program (.MWP) includes.
 *
 *     #include "miniwin.h"
 *     void mwp_entry(void) { ... }          // the ONLY function the linker script puts first: byte 0 of the file
 *
 * A program has no libc and no startup code. Everything it can ask of the kernel is a function pointer in the syscall table
 * (mwp_abi.h), reached through `sys`: sys.fill_rect(...), sys.file_read(...), sys.mem_alloc(...). This header adds the table's
 * address, colours, and a few inline helpers built from those calls.
 *
 * Where it runs: `run NAME.MWP` starts it in a WINDOW (since 1.1-alpha-11); `run -f NAME.MWP` gives it the whole screen. Either way
 * sys.screen_w / sys.screen_h are the size of the area it may draw in -- read them every frame, a window can be resized.
 * Three rules that come from how a program is loaded (the loader copies the file to 0x8F1000 and jumps in; it does not zero
 * anything or set up a heap):
 *   1. No uninitialised globals: there is no .bss. Keep state in locals, or in memory from sys.mem_alloc (zero-filled).
 *   2. At most 24 KB of code + data (PROG_MAX_BYTES). `-Os` and the linker's gc-sections are on; avoid big tables.
 *   3. Return from mwp_entry() when done -- the kernel takes the screen back and frees any memory you forgot to free.
 */
#include "mwp_abi.h"

#define MW_SDK_VERSION 2       /* the SDK itself (this header, mw_libc.h, mw_gfx.h, mw_ui.h, the build tools); sdk/VERSION. Not the ABI: that is sys.abi_version */

#define sys (*(mwp_syscalls_t *)MWP_SYSCALL_TABLE_ADDR)

/* Colours: 0x00RRGGBB, the format of every draw call. (The Classic theme's own palette, so a program matches the desktop.) */
#define MW_BLACK     0x000000u
#define MW_WHITE     0xFFFFFFu
#define MW_BLUE      0x0000AAu
#define MW_FACE      0xC0C0C0u      /* the grey of buttons and window bodies */
#define MW_SHADOW    0x808080u
#define MW_ACCENT    0x000080u      /* title bars, selections */
#define MW_DESKTOP   0x55FFFFu
#define MW_RED       0xAA0000u
#define MW_GREEN     0x00AA00u
#define MW_YELLOW    0xFFFF55u
/* the old name from programs/mwp_api.h, kept so existing programs still build */
#define MWP_COL_BLACK MW_BLACK
#define MWP_COL_WHITE MW_WHITE
#define MWP_COL_BLUE  MW_BLUE

#define MW_KEY_ESC   27
#define MW_KEY_ENTER '\n'
#define MW_KEY_BACKSPACE '\b'

#define MW_FONT_W 11                /* every glyph is 11 x 11 pixels; text_width gives the width of a whole string */
#define MW_FONT_H 11

/* The kernel's ABI version as this program should see it: kernels older than alpha-10 never wrote the field. */
static inline u32 mw_abi(void) { u32 v = sys.abi_version; return (v >= 1 && v < 1000u) ? v : 1u; }
/* 1 if the running kernel has at least version `v` of the table. Check before calling anything added after version 1. */
static inline int mw_has(u32 v) { return mw_abi() >= v; }

/* 1 if the program is running in a window (ABI 3+; older kernels always ran programs full-screen) */
static inline int mw_windowed(void) { return mw_abi() >= 3 && (sys.run_flags & MW_RUN_WINDOWED) != 0; }

static inline int mw_str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }     /* (mw_strlen in mw_libc.h is the same) */
static inline void mw_text(int x, int y, const char *s, u32 color) { sys.draw_string_ko(x, y, s, color); }
static inline void mw_fill_screen(u32 color) { sys.fill_rect(0, 0, (int)sys.screen_w, (int)sys.screen_h, color); }   /* the drawing area: the window's client, or the screen */
static inline int mw_in_rect(int px, int py, int x, int y, int w, int h) { return px >= x && py >= y && px < x + w && py < y + h; }
/* blocks (polling) until a key arrives; returns it */
static inline int mw_wait_key(void) { int k; while ((k = sys.key_poll()) == 0) { } return k; }
/* "123" for 123, in buf (at least 12 bytes); returns buf */
static inline char *mw_itoa(int v, char *buf) {
    char tmp[12]; int n = 0, neg = v < 0; unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    do { tmp[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    int i = 0; if (neg) buf[i++] = '-';
    while (n) buf[i++] = tmp[--n];
    buf[i] = 0;
    return buf;
}
/* appends src to dst at *len (dst must have room: no bounds check) */
static inline void mw_append(char *dst, int *len, const char *src) { while (*src) dst[(*len)++] = *src++; dst[*len] = 0; }

/* The rest of the SDK. Define MW_LITE before including this header to leave them out (and keep a tiny program tiny -- though the
 * linker already drops what a program does not call). */
#ifndef MW_LITE
#include "mw_libc.h"
#include "mw_gfx.h"
#include "mw_ui.h"
#endif

#endif
