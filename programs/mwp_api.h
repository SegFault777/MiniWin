#ifndef MWP_API_H
#define MWP_API_H

/* Minimal freestanding type aliases -- a .mwp has no libc, same as the
 * kernel itself (see kernel/io.h, which this deliberately doesn't
 * include: a program built against kernel/io.h would silently start
 * depending on whatever else that header drags in, and the whole point
 * of this file is to be the ONE thing a .mwp includes, keeping its
 * actual contract with the kernel down to just the syscall table
 * below). */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;

/* Mirrors kernel/mwp.h's mwp_syscalls_t field-for-field -- this is the
 * program side of the same fixed-address contract described there.
 * The two structs MUST stay in sync by hand (there's no shared build
 * step generating both from one source of truth); if you add a
 * syscall, add it to both this file and kernel/mwp.h, in the same
 * order, or the offsets a compiled .mwp expects and the offsets the
 * kernel actually filled in will quietly disagree. */
typedef struct {
    void (*put_pixel)(int x, int y, u32 color);
    void (*fill_rect)(int x, int y, int w, int h, u32 color);
    void (*draw_rect)(int x, int y, int w, int h, u32 color);
    void (*draw_string)(int x, int y, const char *s, u32 color);
    void (*draw_string_ko)(int x, int y, const char *s, u32 color);
    void (*present)(void);
    int  (*key_poll)(void);
    u32  screen_w;
    u32  screen_h;
} mwp_syscalls_t;

#define MWP_SYSCALL_TABLE_ADDR 0x84C00u /* must match kernel/mwp.h exactly */
#define sys (*(mwp_syscalls_t *)MWP_SYSCALL_TABLE_ADDR)

/* A few colors in the same 0x00RRGGBB truecolor format kernel/vga.h's
 * COL_* constants use -- not pulling in vga.h itself (same reasoning as
 * not including io.h above: a .mwp's entire kernel-facing surface is
 * this one file). Add more here as programs need them rather than
 * guessing every color a future program might ever want. */
#define MWP_COL_BLACK  0x000000u
#define MWP_COL_WHITE  0xFFFFFFu
#define MWP_COL_BLUE   0x0000AAu

#endif
