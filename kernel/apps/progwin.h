/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_APPS_PROGWIN_H
#define MW_APPS_PROGWIN_H

/* apps/progwin.h -- a .MWP running IN A WINDOW (1.1-alpha-11). `run NAME.MWP` (MiniShell), double-clicking a program on the desktop
 * and opening one in MiniFiles all start the program here; `run -f NAME.MWP` still gives it the whole screen the old way.
 *
 * THE IDEA. The OS has one CPU thread and no timer interrupt, so the program cannot be preempted -- but it does not have to be. It
 * runs as a COROUTINE: its own stack, started by mw_coro_switch() (coro.h), and it hands the CPU back at the three calls where a
 * well-behaved program waits anyway:
 *     sys.present()   "this frame is done"        -- the kernel blits the program's surface into the window
 *     sys.key_poll()  when nothing is waiting, or after a few polls in a row (a `while (!key_poll())` loop stays cooperative)
 *     sys.sleep_ms()  until the time has passed
 * The main loop (kmain) resumes the program once per pass, so it advances one frame per desktop frame and the desktop, the other
 * windows, the network and the mouse keep working while it runs. A program written for the full screen runs here UNCHANGED.
 *
 * WHERE IT DRAWS. Every drawing syscall ends in the kernel's bb_* functions, which write through the pointer `backbuf` (vga.h). While
 * the program runs, that pointer is aimed at the program's own 640x480 surface (a full-size one so that nothing it draws, however
 * wrong, can fall outside the allocation); the window shows the top-left part of it, the size of the client area, and that size is
 * what the program reads as sys.screen_w / screen_h -- resize the window and the program sees the new size on its next frame.
 *
 * LIMITS, said plainly:
 *  - ONE program window at a time: every .MWP is linked for the same load address (kernel/mwp_link.ld), so two cannot be in memory at
 *    once. (Per-program address spaces -- the paging that Ring 3 needs anyway -- is what lifts this.)
 *  - Cooperative, not protected: a program that loops without ever calling present/key_poll/sleep freezes the OS, and one that writes
 *    through a wild pointer can still crash it. Closing the window kills the program on the spot, wherever it had stopped.
 *  - Runs in ring 0. Ring 3 is the next step; this file is what a ring-3 program window will reuse (surface, input queue, window).
 * Needs: coro.h, mm.h, mwp.h, ui/widgets.h, ui/icon16.h, the window system (windows.h's WIN_ID_PROG; window.h's pw_win). */

#define PW_STACK_BYTES (128u * 1024u)         /* kernel code the program calls (file system, drawing) runs on this stack too */
#define PW_SURF_BYTES  ((u32)VGA_WIDTH * (u32)VGA_HEIGHT * 4u)
#define PW_KEYQ        32

typedef struct {
    int used, finished;
    char name[24];
    mwp_entry_fn entry;
    u32 *surface;                              /* VGA_WIDTH x VGA_HEIGHT pixels */
    u8  *stack, *stack_alloc;                  /* stack_alloc = the pages as allocated: the lowest one is the guard page under the stack */
    int  faulted; u32 fault_vec, fault_err, fault_eip, fault_cr2;   /* the CPU exception that stopped the program, if one did */
    u32  sp, caller_sp;                        /* saved stack pointers of the program and of the kernel while the other runs */
    u8   keyq[PW_KEYQ]; int kh, kt;            /* keys typed while the window had focus */
    int  mx, my, mb, mchg, capture;            /* the pointer in client coordinates; capture = a drag that began inside */
    int  spin;                                 /* key_poll calls since the program last yielded */
    int  slot;                                 /* its /progs slot (for its packed icon and title), -1 if unknown */
    int  fixed_icon;                           /* 1.1-alpha-13: an in-kernel window (the installer) names its title-bar icon here (ui/icon16.h id + 1; 0 = none) */
    char args[64];                             /* `run NAME args...`: the text after the name */
    u32  icon16[256]; int has_icon;            /* set at run time with sys.set_icon */
    mwp_mem_t mem;
} pw_t;

static pw_t  pw;
static pw_t *pw_cur = 0;                       /* non-null exactly while program code is executing */

/* The pixels behind a custom icon id (ui/icon16.h): a program's packed icon, or the one the running program set. */
static const u32 *ic16_custom_ptr(int idx) {
    if (idx == IC_RUNTIME) return (pw.used && pw.has_icon) ? pw.icon16 : 0;
    if (idx >= IC_CUSTOM_BASE && idx < IC_CUSTOM_BASE + PROG_MAX_SLOTS) return mwp_icon16_of(idx - IC_CUSTOM_BASE);
    return 0;
}
/* The icon id the program window and its task bar button show: set at run time > packed into the file > the generic program icon. */
static int pw_icon_id(void) {
    if (pw.used && pw.fixed_icon) return pw.fixed_icon - 1;
    if (pw.used && pw.has_icon) return IC_RUNTIME;
    if (pw.used && pw.slot >= 0 && mwp_icon16_of(pw.slot)) return IC_CUSTOM_BASE + pw.slot;
    return IC_PROG;
}

/* ---------------------------------------------------------------- geometry */
static inline int pw_cx(void) { return pw_win.x + 1; }
static inline int pw_cy(void) { return pw_win.y + TITLEBAR_H + 2; }
static inline int pw_cw(void) { int w = pw_win.w - 2; return w > VGA_WIDTH ? VGA_WIDTH : (w < 1 ? 1 : w); }
static inline int pw_ch(void) { int h = pw_win.h - TITLEBAR_H - 3; return h > VGA_HEIGHT ? VGA_HEIGHT : (h < 1 ? 1 : h); }
static inline int pw_btn_close_x(void) { return win_btn_close_x(&pw_win, 2); }
static inline int pw_btn_max_x(void)   { return win_btn_max_x(&pw_win, 2); }
static inline int pw_btn_min_x(void)   { return win_btn_min_x(&pw_win, 2); }
static inline int pw_btn_y(void)       { return win_btn_y(&pw_win, 1); }
static int pw_close_hit(int px, int py) { return in_rect(px, py, pw_btn_close_x(), pw_btn_y(), BTN_W, BTN_H); }
static int pw_min_hit(int px, int py)   { return in_rect(px, py, pw_btn_min_x(), pw_btn_y(), BTN_W, BTN_H); }
static int pw_max_hit(int px, int py)   { return in_rect(px, py, pw_btn_max_x(), pw_btn_y(), BTN_W, BTN_H); }
static int pw_titlebar_drag_hit(int px, int py) {
    if (!in_rect(px, py, pw_win.x + 1, pw_win.y + 1, pw_win.w - 2, TITLEBAR_H)) return 0;
    return !(pw_min_hit(px, py) || pw_max_hit(px, py) || pw_close_hit(px, py));
}
static int pw_client_hit(int px, int py) { return in_rect(px, py, pw_cx(), pw_cy(), pw_cw(), pw_ch()); }

/* ---------------------------------------------------------------- the program's side of the three waiting calls */
/* Called from inside the program (through the syscall wrappers in mwp_api2.h): give the CPU back until the kernel resumes it. */
static void pw_yield(void) { pw.spin = 0; mw_coro_switch(&pw.sp, pw.caller_sp); }

static void pw_push_key(int k) {
    if (!pw.used || k <= 0 || k > 255) return;
    int next = (pw.kt + 1) % PW_KEYQ;
    if (next == pw.kh) return;                 /* full: drop the newest, as a keyboard buffer does */
    pw.keyq[pw.kt] = (u8)k; pw.kt = next;
}
static int pw_pop_key(void) {
    if (pw.kh == pw.kt) return 0;
    int k = pw.keyq[pw.kh]; pw.kh = (pw.kh + 1) % PW_KEYQ;
    return k;
}

/* ---------------------------------------------------------------- life cycle */
static void pw_trampoline(void) {
    pw.entry();                                /* runs the program until its mwp_entry() returns */
    pw.finished = 1;
    for (;;) mw_coro_switch(&pw.sp, pw.caller_sp);   /* never resumed again: pw_step() closes the window */
}

static void pw_free_run(void) {                /* give back everything this run owns */
    inst_gui_owns = 0;                         /* the INSTALL window (apps/installer.h) is the only thing that sets it, and it is the program window */
    speaker_off();                             /* the program may have been killed in the middle of sys.beep */
    for (int i = 0; i < MWP_ALLOC_MAX; i++) if (pw.mem.p[i]) { kfree(pw.mem.p[i]); pw.mem.p[i] = 0; }
    pw.mem.bytes = 0;
    if (pw.stack_alloc) { paging_unguard(pw.stack_alloc); mm_pages_free(pw.stack_alloc, PW_STACK_BYTES / MM_PAGE + 1); pw.stack_alloc = 0; pw.stack = 0; }
    if (pw.surface) { mm_pages_free(pw.surface, (PW_SURF_BYTES + MM_PAGE - 1) / MM_PAGE); pw.surface = 0; }
}
static void pw_close(void) {
    if (!pw.used) return;
    if (pw_cur) return;                        /* never from inside the program itself */
    pw_free_run();
    pw.used = 0; pw.finished = 0;
    pw_win.open = 0; pw_win.minimized = 0;
    win_z_remove(WIN_ID_PROG);
}

/* Starts program `slot` in the window. Returns 0, or a sentence saying why not. */
static const char *pw_launch_args(int slot, const char *name, const char *args) {
    if (pw.used) return "A program is already running in a window.";
    mwp_entry_fn entry = mwp_load(slot);
    if (!entry) return mwp_load_error ? mwp_load_error : "Failed to load or run.";
    pw.surface = (u32 *)mm_pages_alloc((PW_SURF_BYTES + MM_PAGE - 1) / MM_PAGE);
    pw.stack_alloc = (u8 *)mm_pages_alloc(PW_STACK_BYTES / MM_PAGE + 1);
    if (pw.stack_alloc) { paging_guard(pw.stack_alloc); pw.stack = pw.stack_alloc + MM_PAGE; }    /* the page under the stack faults when touched: a runaway recursion stops here */
    if (!pw.surface || !pw.stack_alloc) { pw_free_run(); return "Not enough memory to start it."; }
    for (u32 i = 0; i < (u32)VGA_WIDTH * VGA_HEIGHT; i++) pw.surface[i] = TH_FACE;     /* a blank window starts in the dialog grey */
    for (int i = 0; i < MWP_ALLOC_MAX; i++) pw.mem.p[i] = 0;
    pw.mem.bytes = 0;
    pw.entry = entry;
    pw.slot = slot; pw.has_icon = 0; pw.fixed_icon = 0;
    { int k = 0; if (args) while (args[k] && k < (int)sizeof(pw.args) - 1) { pw.args[k] = args[k]; k++; } pw.args[k] = 0; }
    pw.used = 1; pw.finished = 0; pw.faulted = 0; pw.kh = pw.kt = 0; pw.spin = 0;
    pw.mx = pw.my = pw.mb = pw.mchg = pw.capture = 0;
    const char *shown = (slot >= 0 && mwp_meta[slot].title[0]) ? mwp_meta[slot].title : name;     /* the program's own title, when it has one */
    mwp_meta_ensure(slot); shown = (slot >= 0 && mwp_meta[slot].title[0]) ? mwp_meta[slot].title : name;
    int n = 0; while (shown[n] && n < (int)sizeof(pw.name) - 1) { pw.name[n] = shown[n]; n++; }
    pw.name[n] = 0;
    pw.caller_sp = 0;
    pw.sp = coro_prepare(pw.stack, PW_STACK_BYTES, pw_trampoline);
    pw_win.open = 1; pw_win.minimized = 0;
    win_z_raise(WIN_ID_PROG);
    return 0;
}

/* 1.1-alpha-13: runs a function that is part of the KERNEL in the program window (the live disc's installer, apps/installer.h). It is a
 * coroutine exactly like a loaded .MWP -- same surface, same stack, same pw_yield() at the end of each frame, same title bar, task bar
 * button, minimise / maximise / close -- but it needs no file and may call kernel functions directly. Returns 0, or a sentence saying
 * why not. `icon` is a ui/icon16.h id for the title bar and the task bar. */
static const char *pw_launch_fn(mwp_entry_fn entry, const char *title, int icon) {
    if (pw.used) return "A program is already running in a window.";
    pw.surface = (u32 *)mm_pages_alloc((PW_SURF_BYTES + MM_PAGE - 1) / MM_PAGE);
    pw.stack_alloc = (u8 *)mm_pages_alloc(PW_STACK_BYTES / MM_PAGE + 1);
    if (pw.stack_alloc) { paging_guard(pw.stack_alloc); pw.stack = pw.stack_alloc + MM_PAGE; }
    if (!pw.surface || !pw.stack_alloc) { pw_free_run(); return "Not enough memory to start it."; }
    for (u32 i = 0; i < (u32)VGA_WIDTH * VGA_HEIGHT; i++) pw.surface[i] = TH_FACE;
    for (int i = 0; i < MWP_ALLOC_MAX; i++) pw.mem.p[i] = 0;
    pw.mem.bytes = 0;
    pw.entry = entry;
    pw.slot = -1; pw.has_icon = 0; pw.fixed_icon = icon + 1;
    pw.args[0] = 0;
    pw.used = 1; pw.finished = 0; pw.faulted = 0; pw.kh = pw.kt = 0; pw.spin = 0;
    pw.mx = pw.my = pw.mb = pw.mchg = pw.capture = 0;
    int n = 0; while (title[n] && n < (int)sizeof(pw.name) - 1) { pw.name[n] = title[n]; n++; }
    pw.name[n] = 0;
    pw.caller_sp = 0;
    pw.sp = coro_prepare(pw.stack, PW_STACK_BYTES, pw_trampoline);
    pw_win.open = 1; pw_win.minimized = 0; pw_win.maximized = 0;
    pw_win.x = pw_win.restore_x = 90; pw_win.y = pw_win.restore_y = 20; pw_win.w = pw_win.restore_w = 444; pw_win.h = pw_win.restore_h = 395;
    win_z_raise(WIN_ID_PROG);
    return 0;
}

static int pw_running(void) { return pw.used; }
static const char *pw_launch(int slot, const char *name) { return pw_launch_args(slot, name, ""); }

/* The name a program's slot has in /progs (what the title bar shows). */
static const char *prog_name_of(int slot) { return (slot >= 0 && slot < PROG_MAX_SLOTS) ? prog_names[slot] : ""; }

/* Is the program's window the one in front (the one that gets the keys and the mouse)? */
static int pw_is_front(void) {
    for (int i = z_count - 1; i >= 0; i--) {
        int id = z_order[i];
        if (win_is_open(id) && !win_is_minimized(id)) return id == WIN_ID_PROG;
    }
    return 0;
}

/* Once per main-loop pass: let the program run until it yields. (mx,my) is the pointer on the screen. */
static char pw_notice[72];
static const char *pw_step(int mx, int my, int left, int right) {
    if (!pw.used || pw.finished) { if (pw.used && pw.finished) pw_close(); return 0; }
    int front = pw_is_front(), inside = pw_client_hit(mx, my);
    if (!left) pw.capture = 0;
    else if (front && inside && !pw.capture) pw.capture = 1;
    int cx = mx - pw_cx(), cy = my - pw_cy();
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= pw_cw()) cx = pw_cw() - 1;
    if (cy >= pw_ch()) cy = pw_ch() - 1;
    int mb = (pw.capture ? 1 : 0) | ((front && inside && right) ? 2 : 0);
    if (cx != pw.mx || cy != pw.my || mb != pw.mb) pw.mchg = 1;
    pw.mx = cx; pw.my = cy; pw.mb = mb;

    mwp_syscalls.screen_w = (u32)pw_cw(); mwp_syscalls.screen_h = (u32)pw_ch();
    mwp_syscalls.run_flags = MW_RUN_WINDOWED; mwp_syscalls.args = pw.args;
    u32 *saved = mw_bb_ptr;
    mw_bb_ptr = pw.surface; pw_cur = &pw; mwp_mem = &pw.mem;
    mw_coro_switch(&pw.caller_sp, pw.sp);      /* ... the program runs here, until it calls present / key_poll / sleep_ms or returns */
    mw_bb_ptr = saved; pw_cur = 0; mwp_mem = &mwp_mem_legacy;
    mwp_syscalls.screen_w = VGA_WIDTH; mwp_syscalls.screen_h = VGA_HEIGHT;
    mwp_syscalls.run_flags = 0; mwp_syscalls.args = "";
    const char *notice = 0;
    if (pw.finished && pw.faulted) {                      /* the program was stopped by a CPU exception: say which, and where */
        static const char *const names[20] = { "DIVIDE ERROR", "DEBUG", "NMI", "BREAKPOINT", "OVERFLOW", "BOUND RANGE", "INVALID OPCODE", "NO FPU", "DOUBLE FAULT (STACK OVERFLOW?)",
            "COPROCESSOR", "INVALID TSS", "SEGMENT", "STACK FAULT", "GENERAL PROTECTION", "PAGE FAULT", "RESERVED", "FPU ERROR", "ALIGNMENT", "MACHINE CHECK", "SIMD" };
        u32 l = 0; char h[9];
        pw_notice[0] = 0;
        append_str(pw_notice, &l, sizeof pw_notice, "PROGRAM STOPPED: ");
        append_str(pw_notice, &l, sizeof pw_notice, pw.fault_vec < 20 ? names[pw.fault_vec] : "EXCEPTION");
        if (pw.fault_vec == 14) { append_str(pw_notice, &l, sizeof pw_notice, " AT "); fatal_hex(h, pw.fault_cr2, 8); append_str(pw_notice, &l, sizeof pw_notice, h); }
        append_str(pw_notice, &l, sizeof pw_notice, " EIP "); fatal_hex(h, pw.fault_eip, 8); append_str(pw_notice, &l, sizeof pw_notice, h);
        serial_puts("[MWP] "); serial_puts(pw_notice); serial_puts("\r\n");
        notice = pw_notice;
    }
    if (pw.finished) pw_close();
    return notice;
}

/* The CPU raised an exception (cpu_fault() in fatal.h calls this first). If a program in the window was running, stop it: record what
 * happened and switch back to the kernel's stack for good -- the stack the fault happened on (possibly the very one that overflowed) is
 * abandoned and freed by pw_close(). Returns 0 only when the fault was not the program's. */
static int pw_fault(const u32 *f) {
    if (!pw_cur) return 0;
    pw.fault_vec = f[8]; pw.fault_err = f[9]; pw.fault_eip = f[10];
    __asm__ volatile ("movl %%cr2, %0" : "=r"(pw.fault_cr2));
    pw.faulted = 1; pw.finished = 1;
    mw_coro_switch(&pw.sp, pw.caller_sp);
    return 1;                                                  /* never reached */
}

/* ---------------------------------------------------------------- drawing */
static void draw_prog_window(void) {
    ui_window_frame_ic(pw_win.x, pw_win.y, pw_win.w, pw_win.h, pw_win.maximized, pw.used ? pw.name : "PROGRAM", pw_icon_id());
    ui_titlebar_buttons(pw_btn_min_x(), pw_btn_max_x(), pw_btn_close_x(), pw_btn_y(),
                        pressed_btn_kind == BTN_MIN && pressed_btn_win == WIN_ID_PROG,
                        pressed_btn_kind == BTN_MAX && pressed_btn_win == WIN_ID_PROG,
                        pressed_btn_kind == BTN_CLOSE && pressed_btn_win == WIN_ID_PROG);
    if (!pw.used || !pw.surface) return;
    int cx = pw_cx(), cy = pw_cy(), cw = pw_cw(), ch = pw_ch();
    for (int y = 0; y < ch; y++) {
        int sy = cy + y;
        if (sy < 0 || sy >= VGA_HEIGHT) continue;
        const u32 *src = &pw.surface[(u32)y * VGA_WIDTH];
        u32 *dst = &MW_REAL_BACKBUF[(u32)sy * VGA_WIDTH];
        int x0 = 0, x1 = cw;
        if (cx < 0) x0 = -cx;
        if (cx + x1 > VGA_WIDTH) x1 = VGA_WIDTH - cx;
        for (int x = x0; x < x1; x++) dst[cx + x] = src[x];
    }
}

#endif
