/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MWP_H
#define MWP_H
#include "io.h"
#include "memmap.h"
#include "vga.h"
#include "font.h"
#include "font_ko.h"
#include "fs.h"
#include "serial.h"
#include "../sdk/include/mwp_abi.h"   /* the syscall table's layout, shared with the program SDK */

/* ============================================================
 * .mwp loader -- MiniWin's answer to "can I run code I didn't compile
 * into the kernel." Short version: yes, but the honest way, not the
 * pretend way.
 *
 * This kernel has no paging, no ring 3, no ELF, no relocation, and no
 * intention of growing any of those today -- it's one flat binary
 * loaded at a fixed address by boot/stage2.asm and running entirely in
 * ring 0 with a single flat address space. A ".mwp" program is built
 * from exactly the same mold: a small, position-independent, no-libc
 * flat binary that this loader copies to a fixed address in RAM and
 * jumps to, precisely the same maneuver stage2 already pulls on the
 * kernel itself at boot. There is no memory protection whatsoever
 * between a running .mwp and the kernel that loaded it -- a buggy
 * program can scribble over kernel state, jump into garbage, or hang
 * the machine, exactly as easily as a bug in kernel.c itself could.
 * That's not an oversight; it's what "no paging" *means*. A real OS
 * would put a program in its own address space with its own page
 * tables and let the CPU's protection rings do the enforcing. MiniWin
 * doesn't have page tables, so it doesn't get to pretend it has
 * isolation -- running a .mwp is exactly as trusted as running more
 * kernel code, because that is, mechanically, exactly what it is.
 *
 * What a .mwp file actually is: raw x86 machine code, compiled
 * freestanding (no libc, no startup files -- see tools/build_mwp.sh)
 * and linked to run starting at MWP_LOAD_ADDR (see below), with its
 * very first byte (plus entry_offset, almost always 0) being the first
 * instruction to execute. No headers of its own -- the header lives
 * separately in the program's disk slot (see fs.h's prog_save_slot());
 * the .mwp bytes on disk are pure code+data, nothing else. It gets one
 * calling convention in (jumped to with no arguments) and one way to
 * talk to the rest of the OS: the syscall table below.
 *
 * Why a syscall table instead of just letting a .mwp call kernel
 * functions directly by address: a .mwp is compiled completely
 * separately from the kernel (different build, potentially a different
 * day entirely), so it can't know kernel.c's actual function addresses
 * -- those shift every time the kernel is rebuilt. What it CAN know,
 * because both sides agree on it as a fixed contract, is one constant
 * address (MWP_SYSCALL_TABLE_ADDR) where it'll always find a table of
 * function pointers, filled in by the kernel right before the jump.
 * Index 0 is always "put a pixel," index 1 is always "draw a string,"
 * and so on -- see mwp_syscalls_t below for the actual list. This is
 * the same fixed-address-contract trick real BIOSes and real bootloaders
 * use to hand off to code they didn't build (a jump table at a known
 * offset beats trying to link two separately-built binaries together),
 * just scaled down to fit an OS with no linker involved at load time
 * at all.
 * ============================================================ */

/* Where a .mwp gets copied to and jumped into: a FIXED spot in high memory (memmap.h explains why -- the old
 * 0x85000 was inside the kernel's own .bss once .bss outgrew it, and every program launch scribbled over the
 * network state). The program linker script (kernel/mwp_link.ld) and programs/mwp_api.h carry hand-kept copies
 * of these two numbers; the asserts below make a mismatch with memmap.h a build error on this side. */
#define MWP_LOAD_ADDR         MW_MWP_LOAD_ADDR

/* One syscall table, filled in once at boot (see mwp_init() below) and
 * read-only from every .mwp's point of view after that. A single
 * global table rather than one per running program because this kernel
 * only ever runs one .mwp at a time anyway (see mwp_run() -- there's no
 * concurrent-programs concept here, no more than there's a concurrent-
 * windows-being-dragged concept); if that ever changes, this table
 * would need to move to per-slot storage, but building that speculatively
 * now would just be dead weight. */
/* (MWP_SYSCALL_TABLE_ADDR itself comes from mwp_abi.h; the assert below proves it equals memmap.h's) */
typedef char mw_assert_mwp_table_addr[(MWP_SYSCALL_TABLE_ADDR == MW_MWP_SYSCALL_ADDR) ? 1 : -1];
typedef char mw_assert_mwp_addrs[(MWP_LOAD_ADDR == 0x8F1000u && MWP_SYSCALL_TABLE_ADDR == 0x8F0000u) ? 1 : -1];

/* The table's layout lives in sdk/include/mwp_abi.h, shared with the program side -- see the comments there (and the version
 * history: version 2 arrived in 1.1-alpha-10). The fields are filled in by mwp_init() (the drawing and keyboard ones) and
 * mwp_api2.h's mwp_api2_init() (everything added in version 2). */


/* The syscall table itself lives at a fixed address (see
 * MWP_SYSCALL_TABLE_ADDR) so a separately-compiled .mwp can find it
 * without needing kernel.c's actual addresses -- see this file's
 * top-of-file comment for why. Casting a bare integer to a struct
 * pointer is exactly as sketchy as it looks; it's the one deliberate
 * exception to this codebase's normal type discipline, made because
 * "a fixed address both sides agree on" IS the entire ABI here. */
#define mwp_syscalls (*(mwp_syscalls_t *)MWP_SYSCALL_TABLE_ADDR)

/* Fills in the syscall table. Called once at boot, well before any
 * program could possibly run -- see kernel_main()'s init sequence. */
static inline void mwp_init(void) {
    mwp_syscalls_t *t = (mwp_syscalls_t *)MWP_SYSCALL_TABLE_ADDR;
    t->put_pixel     = bb_putpixel;
    t->fill_rect     = bb_fillrect;
    t->draw_rect     = bb_rect;
    t->draw_string   = font_draw_string;
    t->draw_string_ko = ko_draw_mixed_string;
    t->present       = vga_present;
    t->key_poll      = 0; /* wired up from kernel.c, which owns the
                           * keyboard IRQ buffer this would need to
                           * drain -- mwp.h itself doesn't touch hardware
                           * state directly, same separation as every
                           * other kernel.c <-> *.h relationship in this
                           * codebase */
    t->screen_w      = VGA_WIDTH;
    t->screen_h      = VGA_HEIGHT;
}

/* The blocks a running program has asked for (mem_alloc). One table per run: the full-screen run uses mwp_mem_legacy, the program in
 * a window carries its own (apps/progwin.h) and points mwp_mem at it while it executes. mwp_api2.h does the allocating. */
#define MWP_ALLOC_MAX 64
typedef struct { void *p[MWP_ALLOC_MAX]; u32 bytes; } mwp_mem_t;
static mwp_mem_t mwp_mem_legacy;
static mwp_mem_t *mwp_mem = &mwp_mem_legacy;

typedef void (*mwp_entry_fn)(void);
/* Runs right after a program returns (mwp_api2.h frees the memory the program left allocated). */
static void (*mwp_on_exit)(void) = 0;
static void (*mwp_on_start)(void) = 0;

/* Loads program `slot` into MWP_LOAD_ADDR and jumps to it. Returns 0
 * (and doesn't jump anywhere) if the slot's empty or something about it
 * looks wrong -- the one thing this loader refuses to do is jump into
 * memory it never actually loaded a program into. Once it DOES jump,
 * though, all bets are off in the way this file's top comment already
 * explained: there is no returning-with-an-error-code from a crashed
 * .mwp, because there is no protection boundary to catch a crash at.
 * A .mwp is expected to return normally (its entry function just
 * reaching its own closing brace) when it's done -- at which point
 * control comes back here and mwp_run() returns 1 -- not to expect the
 * kernel to survive it doing anything else. */
#include "mwp_trailer.h"                    /* the optional icon/title trailer: parsing and validation (host-tested) */
static const char *mwp_load_error = 0;           /* why the last mwp_load() said no, when it is more than "the slot is bad" */

/* Copies program `slot` to MWP_LOAD_ADDR and returns its entry function, or 0 if the slot is empty or anything about it looks wrong
 * (see mwp_run's comment: this loader never hands out an entry point into memory it did not fill completely). */
static inline mwp_entry_fn mwp_load(int slot) {
    u32 len = 0, entry_offset = 0;
    if (!prog_check_slot(slot, &len, &entry_offset, 0)) return 0;
    if (len == 0 || entry_offset >= len) return 0;

    u8 *dst = (u8 *)MWP_LOAD_ADDR;
    u32 got = prog_load_slot(slot, dst, PROG_MAX_BYTES);
    /* Exactly the whole program or no program at all: the entry point
     * lives somewhere inside `len` bytes, and jumping into a region that
     * was only partly filled is how you execute whatever RAM happened to
     * contain. (prog_load_slot() already returns 0 for any short read;
     * this check is the belt to that function's suspenders.) */
    if (got == 0 || got != len) return 0;
    mwp_load_error = 0;
    mwp_trailer_t tr;
    if (mwp_trailer_parse(dst, got, &tr)) {                          /* a program with a trailer: icon/title are the kernel's business, not code */
        if (tr.min_abi > MWP_ABI_VERSION) { mwp_load_error = "That program needs a newer MiniWin."; return 0; }
        if (entry_offset >= tr.payload_len) return 0;
    }
    return (mwp_entry_fn)(MWP_LOAD_ADDR + entry_offset);
}

/* The FULL-SCREEN run (`run -f`): loads program `slot` and calls it on the kernel's own stack; the program owns the screen until it
 * returns. Returns 0 (and doesn't jump anywhere) if the slot's empty or something about it looks wrong. Once it DOES jump, all bets
 * are off in the way this file's top comment already explained: there is no returning-with-an-error-code from a crashed .mwp,
 * because there is no protection boundary to catch a crash at. A .mwp is expected to return normally (its entry function just
 * reaching its own closing brace) when it's done -- at which point control comes back here and mwp_run() returns 1.
 * (The default `run` starts programs in a WINDOW instead: apps/progwin.h.) */
static inline int mwp_run(int slot) {
    mwp_entry_fn entry = mwp_load(slot);
    if (!entry) return 0;
    if (mwp_on_start) mwp_on_start();
    entry();
    if (mwp_on_exit) mwp_on_exit();
    return 1;
}

#endif
