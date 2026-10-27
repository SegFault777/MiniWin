/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_MWP_API2_H
#define MW_MWP_API2_H

/* mwp_api2.h -- the kernel's side of the syscall table's version-2 functions (layout: sdk/include/mwp_abi.h; the program's
 * view of them: sdk/include/miniwin.h and sdk/README.md).
 *
 * Each function here is a thin, defensive wrapper: a program is ring-0 code nobody reviewed, so every pointer and length it
 * passes is checked as far as a flat address space allows (non-null, sizes capped) and every filesystem error is mapped to a
 * negative number the program can pass to err_text. What a wrapper CANNOT do is stop a program from writing anywhere it likes
 * with its own pointers -- that needs paging (see mm.h's "what it is not").
 *
 * Memory: mem_alloc hands out kernel-heap blocks and remembers them (64 per program run). When the program returns, mwp_run()
 * calls mwp_api2_release() and every block still held is freed -- a leaking program cannot slowly eat the heap of the OS.
 * Needs (included before this): mm.h, vfs.h, rtc.h, delay.h, sysinfo.h, serial.h, ui/icon16.h, ui/widgets.h, mouse.h, font_ko.h,
 * apps/minifiles.h (fx_resync_desktop: a file written into /docs or /progs must show up on the desktop). */

#define MWP_ALLOC_CAP (4u * 1024u * 1024u)            /* one run may hold at most 4 MB */

static void *mwp_sys_alloc(u32 n) {
    if (n == 0 || n > MWP_ALLOC_CAP || mwp_mem->bytes + n > MWP_ALLOC_CAP) return 0;
    for (int i = 0; i < MWP_ALLOC_MAX; i++) {
        if (mwp_mem->p[i]) continue;
        void *p = kzalloc(n);
        if (!p) return 0;
        mwp_mem->p[i] = p; mwp_mem->bytes += mm_usable(p);
        return p;
    }
    return 0;
}
static int mwp_sys_free(void *p) {
    if (!p) return 0;
    for (int i = 0; i < MWP_ALLOC_MAX; i++) {
        if (mwp_mem->p[i] != p) continue;
        u32 u = mm_usable(p);
        mwp_mem->p[i] = 0; mwp_mem->bytes = mwp_mem->bytes >= u ? mwp_mem->bytes - u : 0;
        return kfree(p);
    }
    return 0;                                         /* not ours: a program may not free the kernel's blocks */
}
static u32 mwp_sys_mem_free_kb(void) { mm_stats_t s; mm_stats(&s); return s.free_pages * 4u; }
/* A full-screen run starts with nothing; when it returns, whatever it still holds goes back. (A windowed run's table is freed by
 * pw_close() in apps/progwin.h.) */
static void mwp_api2_start(void) { for (int i = 0; i < MWP_ALLOC_MAX; i++) mwp_mem_legacy.p[i] = 0; mwp_mem_legacy.bytes = 0; }
static void mwp_api2_release(void) {
    int leaked = 0;
    for (int i = 0; i < MWP_ALLOC_MAX; i++) if (mwp_mem_legacy.p[i]) { kfree(mwp_mem_legacy.p[i]); mwp_mem_legacy.p[i] = 0; leaked++; }
    mwp_mem_legacy.bytes = 0;
    if (leaked) { char m[48]; u32 l = 0; m[0] = 0; append_str(m, &l, sizeof m, "[MWP] freed "); append_uint(m, &l, sizeof m, leaked); append_str(m, &l, sizeof m, " leaked block(s)"); serial_puts(m); serial_puts("\r\n"); }
}

/* ---------------------------------------------------------------- files */
static int mwp_err(int e) { return e == MWFS_OK ? 0 : -e; }
static int mwp_path_ok(const char *p) {
    if (!p) return 0;
    int n = 0; while (p[n] && n < VFS_PATH_MAX) n++;
    return n > 0 && n < VFS_PATH_MAX;
}
static int mwp_under(const char *path, const char *dir) {            /* path is dir itself or inside it (case-insensitive) */
    char a[VFS_PATH_MAX];
    if (vfs_norm(path, a, sizeof a) != MWFS_OK) return 0;
    int n = 0; while (dir[n]) n++;
    for (int i = 0; i < n; i++) if (fx_up(a[i]) != fx_up(dir[i])) return 0;
    return a[n] == 0 || a[n] == '/';
}
static void mwp_resync_if_slot_dir(const char *path) {
    if (mwp_under(path, "/docs") || mwp_under(path, "/progs")) fx_resync_desktop();
}
static int mwp_sys_file_size(const char *path) {
    vfs_node_t n;
    if (!mwp_path_ok(path)) return -MWFS_E_NAME;
    int e = vfs_stat(path, &n);
    if (e != MWFS_OK) return -e;
    if (n.type != MWFS_T_FILE) return -MWFS_E_ISDIR;
    mwfs_ino_t t;
    if (!mwfs_ino_get(&vfs_vols[n.vol], n.ino, &t)) return -MWFS_E_IO;
    return t.size > 0x7FFFFFFFu ? -MWFS_E_TOOBIG : (int)t.size;
}
static int mwp_sys_file_read(const char *path, void *buf, u32 max) {
    if (!mwp_path_ok(path) || !buf) return -MWFS_E_NAME;
    u32 len = 0;
    int e = vfs_read(path, (u8 *)buf, max, &len, 0);
    if (e != MWFS_OK) return -e;
    return (int)len;
}
static int mwp_sys_file_write(const char *path, const void *data, u32 len) {
    if (!mwp_path_ok(path) || (len && !data)) return -MWFS_E_NAME;
    int e = vfs_write(path, (const u8 *)data, len, 0);
    if (e == MWFS_OK) { fx_free_invalidate(); mwp_resync_if_slot_dir(path); }
    return mwp_err(e);
}
static int mwp_sys_file_delete(const char *path) {
    if (!mwp_path_ok(path)) return -MWFS_E_NAME;
    int e = vfs_rm(path);
    if (e == MWFS_OK) { fx_free_invalidate(); mwp_resync_if_slot_dir(path); }
    return mwp_err(e);
}
static int mwp_sys_dir_make(const char *path) {
    if (!mwp_path_ok(path)) return -MWFS_E_NAME;
    int e = vfs_mkdir(path);
    if (e == MWFS_OK) fx_free_invalidate();
    return mwp_err(e);
}
typedef struct { mw_dir_cb cb; void *ctx; int count; int in_mnt; int vol; } mwp_ls_t;
static int mwp_ls_cb(const mwfs_dirent_t *e, void *c) {
    mwp_ls_t *x = (mwp_ls_t *)c;
    u32 size = 0;
    if (e->type != MWFS_T_DIR) {
        mwfs_vol_t *v = x->in_mnt ? vfs_find_mount(e->name) : &vfs_vols[x->vol];
        mwfs_ino_t t;
        if (v && mwfs_ino_get(v, e->ino, &t)) size = t.size;
    }
    x->count++;
    return x->cb(e->name, e->type == MWFS_T_DIR, size, x->ctx);
}
static int mwp_sys_dir_list(const char *path, mw_dir_cb cb, void *ctx) {
    if (!mwp_path_ok(path) || !cb) return -MWFS_E_NAME;
    vfs_node_t d;
    int e = vfs_stat(path, &d);
    if (e != MWFS_OK) return -e;
    if (d.type != MWFS_T_DIR) return -MWFS_E_NOTDIR;
    mwp_ls_t x; x.cb = cb; x.ctx = ctx; x.count = 0; x.in_mnt = d.is_mnt_dir; x.vol = d.vol;
    if (vfs_ls_each(&d, mwp_ls_cb, &x) < 0) return -MWFS_E_IO;
    return x.count;
}
static const char *mwp_sys_err_text(int err) { return err == 0 ? "ok" : mwfs_err_text(err < 0 ? -err : err); }

/* ---------------------------------------------------------------- time, mouse, misc */
static u32 mwp_tsc_prev = 0, mwp_ms_total = 0, mwp_cycles_per_ms = 0, mwp_ms_started = 0;
static u32 mwp_sys_ticks_ms(void) {
    if (!mwp_ms_started) {
        mwp_ms_started = 1;
        mwp_cycles_per_ms = sysinfo_cpu_mhz() * 1000u;          /* ~20 ms, once */
        mwp_tsc_prev = sysinfo_tsc32();
    }
    if (!mwp_cycles_per_ms) return mwp_ms_total;                 /* no usable TSC: time stands still rather than lies */
    u32 now = sysinfo_tsc32();
    u32 delta = now - mwp_tsc_prev;                              /* wraps correctly while called at least every 2^32 cycles */
    u32 ms = delta / mwp_cycles_per_ms;
    mwp_ms_total += ms; mwp_tsc_prev += ms * mwp_cycles_per_ms;  /* keep the remainder for the next call */
    return mwp_ms_total;
}
static void mwp_sys_sleep_ms(u32 ms) {
    if (ms > 60000u) ms = 60000u;
    if (pw_cur) {                                       /* in a window: let the desktop run until the time is up */
        u32 until = mwp_sys_ticks_ms() + ms;
        do pw_yield(); while ((int)(until - mwp_sys_ticks_ms()) > 0 && mwp_cycles_per_ms);
        return;
    }
    delay_ms(ms);
}
static void mwp_sys_time_now(mw_time_t *t) {
    if (!t) return;
    rtc_time_t r; rtc_read(&r);
    t->year = (u16)r.year; t->month = (u8)r.month; t->day = (u8)r.day; t->hour = (u8)r.hour; t->minute = (u8)r.minute; t->second = (u8)r.second; t->weekday = (u8)r.weekday;
}
/* The keyboard and the mouse share ONE output buffer (the i8042's): while a mouse byte sits there, keyboard_poll_key() sees it
 * and refuses to read, so a program that only watched the keys would go deaf after the first mouse click. The kernel's own main
 * loop polls both for that reason; a program has no such loop, so BOTH of its input calls (key_poll and mouse_poll) drain the
 * mouse first and keep the pointer's position and buttons for whenever mouse_poll is asked. */
static int mwp_mx = 0, mwp_my = 0, mwp_mb = 0, mwp_mchanged = 0;
static void mwp_mouse_pump(void) {
    mouse_poll();
    if (mouse_dx || mouse_dy) mwp_mchanged = 1;
    mwp_mx += mouse_dx; mwp_my += mouse_dy;
    if (mwp_mx < 0) mwp_mx = 0;
    if (mwp_my < 0) mwp_my = 0;
    if (mwp_mx >= VGA_WIDTH) mwp_mx = VGA_WIDTH - 1;
    if (mwp_my >= VGA_HEIGHT) mwp_my = VGA_HEIGHT - 1;
    int b = (mouse_left ? 1 : 0) | (mouse_right ? 2 : 0);
    if (b != mwp_mb) mwp_mchanged = 1;
    mwp_mb = b;
}
static int mwp_sys_mouse_poll(int *x, int *y, int *buttons) {
    if (pw_cur) {                                       /* in a window: the kernel's pointer, already in client coordinates */
        if (x) *x = pw.mx;
        if (y) *y = pw.my;
        if (buttons) *buttons = pw.mb;
        int c = pw.mchg; pw.mchg = 0;
        return c;
    }
    mwp_mouse_pump();
    if (x) *x = mwp_mx;
    if (y) *y = mwp_my;
    if (buttons) *buttons = mwp_mb;
    int c = mwp_mchanged; mwp_mchanged = 0;
    return c;
}
static int mwp_sys_key_poll(void) {
    if (pw_cur) {                                       /* in a window: keys the desktop routed here (only while it has focus) */
        int k = pw_pop_key();
        if (k) { pw.spin = 0; return k; }
        if (++pw.spin >= 4) { pw_yield(); k = pw_pop_key(); }   /* a polling loop gives the desktop its turn */
        return k;
    }
    mwp_mouse_pump(); return keyboard_poll_key();
}
static void mwp_sys_present(void) { if (pw_cur) pw_yield(); else vga_present(); }
static void mwp_sys_set_title(const char *s) {
    if (!pw_cur || !s) return;
    int n = 0; while (s[n] && n < (int)sizeof(pw.name) - 1) { pw.name[n] = s[n]; n++; }
    pw.name[n] = 0;
}
static void mwp_sys_icon16(int x, int y, int icon) { ic16_blit(x, y, icon); }
static void mwp_sys_icon12(int x, int y, int icon) { ic16_blit12(x, y, icon); }
static int  mwp_sys_text_width(const char *s) { return s ? ko_string_width(s) : 0; }
static void mwp_sys_bevel(int x, int y, int w, int h, int pressed) { ui_bevel(x, y, w, h, pressed); }
static void mwp_sys_button(int x, int y, int w, int h, const char *label, int pressed) {
    ui_button(x, y, w, h, label ? label : "", 0, 0, pressed);
}
static void mwp_sys_window_frame(int x, int y, int w, int h, const char *title, int icon) {
    if (icon >= 0) ui_window_frame_ic(x, y, w, h, 0, title ? title : "", icon);
    else ui_window_frame(x, y, w, h, 0, title ? title : "");
}
static void mwp_sys_log(const char *s) { if (s) { serial_puts(s); serial_puts("\r\n"); } }
static u32 mwp_rng = 0x2545F491u;
static u32 mwp_sys_rand32(void) { mwp_rng ^= sysinfo_tsc32(); mwp_rng = mwp_rng * 1664525u + 1013904223u; return mwp_rng ^ (mwp_rng >> 15); }

/* ---------------------------------------------------------------- version 3: clipboard, window, theme, sound */
static int mwp_sys_clip_get(void *buf, u32 max) { if (!buf && max) return 0; return (int)clip_get(buf, max); }
static int mwp_sys_clip_set(const void *data, u32 len) { return clip_set(data, len); }
static void mwp_sys_set_size(int cw, int ch) {
    if (!pw_cur) return;                                             /* a full-screen program already has the whole screen */
    if (cw < 60) cw = 60;
    if (ch < 40) ch = 40;
    if (cw > VGA_WIDTH - 2) cw = VGA_WIDTH - 2;
    if (ch > VGA_HEIGHT - TASKBAR_H - TITLEBAR_H - 3) ch = VGA_HEIGHT - TASKBAR_H - TITLEBAR_H - 3;
    if (pw_win.maximized) return;
    pw_win.w = cw + 2; pw_win.h = ch + TITLEBAR_H + 3;
    if (pw_win.x + pw_win.w > VGA_WIDTH) pw_win.x = VGA_WIDTH - pw_win.w;           /* keep the whole window on the screen */
    if (pw_win.y + pw_win.h > VGA_HEIGHT - TASKBAR_H) pw_win.y = VGA_HEIGHT - TASKBAR_H - pw_win.h;
    if (pw_win.x < 0) pw_win.x = 0;
    if (pw_win.y < 0) pw_win.y = 0;
    pw_win.restore_w = pw_win.w; pw_win.restore_h = pw_win.h; pw_win.restore_x = pw_win.x; pw_win.restore_y = pw_win.y;
}
static void mwp_sys_set_icon(const u32 *argb16) {
    if (!pw_cur || !argb16) return;
    for (int i = 0; i < 256; i++) pw.icon16[i] = argb16[i];
    pw.has_icon = 1;
}
static u32 mwp_sys_theme_color(int id) {
    switch (id) {
        case MW_TH_FACE: return TH_FACE;             case MW_TH_TEXT: return TH_TEXT;
        case MW_TH_ACCENT: return TH_ACCENT;         case MW_TH_ACCENT_TEXT: return TH_ACCENT_TEXT;
        case MW_TH_FIELD: return TH_FIELD;           case MW_TH_FIELD_TEXT: return TH_FIELD_TEXT;
        case MW_TH_LIGHT: return TH_LIGHT;           case MW_TH_SHADOW: return TH_SHADOW;
        case MW_TH_OUTLINE: return TH_OUTLINE;       case MW_TH_DESKTOP: return TH_DESKTOP;
        case MW_TH_WARNING: return TH_WARNING;
    }
    return TH_FACE;
}
static void mwp_sys_beep(u32 hz, u32 ms) {
    if (hz < 20 || hz > 20000) return;
    if (ms > 2000) ms = 2000;
    speaker_set_frequency(hz); speaker_on();
    mwp_sys_sleep_ms(ms);                                            /* cooperative in a window; pw_close() silences the speaker if the window is closed meanwhile */
    speaker_off();
}

typedef char mw_assert_mwi_icons[(MWI_DOC == IC_DOC && MWI_FOLDER == IC_FOLDER && MWI_FOLDER_OPEN == IC_FOLDER_OPEN && MWI_PC == IC_PC && MWI_HTML == IC_HTML &&
                                  MWI_FILES == IC_FILES && MWI_WARNING == IC_WARNING && MWI_UP == IC_UP && MWI_TERMINAL == IC_TERM && MWI_PROGRAM == IC_PROG) ? 1 : -1];

/* Fills in everything added in ABI version 2 (mwp_init() has already done version 1's). */
static void mwp_api2_init(void) {
    mwp_syscalls_t *t = (mwp_syscalls_t *)MWP_SYSCALL_TABLE_ADDR;
    t->abi_version = MWP_ABI_VERSION; t->table_bytes = (u32)sizeof(mwp_syscalls_t);
    t->mem_alloc = mwp_sys_alloc; t->mem_free = mwp_sys_free; t->mem_free_kb = mwp_sys_mem_free_kb;
    t->file_size = mwp_sys_file_size; t->file_read = mwp_sys_file_read; t->file_write = mwp_sys_file_write;
    t->file_delete = mwp_sys_file_delete; t->dir_make = mwp_sys_dir_make; t->dir_list = mwp_sys_dir_list; t->err_text = mwp_sys_err_text;
    t->ticks_ms = mwp_sys_ticks_ms; t->sleep_ms = mwp_sys_sleep_ms; t->time_now = mwp_sys_time_now;
    t->mouse_poll = mwp_sys_mouse_poll; t->key_poll = mwp_sys_key_poll; t->present = mwp_sys_present; t->set_title = mwp_sys_set_title; t->run_flags = 0;     /* key_poll replaces version 1's raw keyboard_poll_key, same meaning */
    t->draw_icon16 = mwp_sys_icon16; t->draw_icon12 = mwp_sys_icon12; t->text_width = mwp_sys_text_width;
    t->bevel = mwp_sys_bevel; t->button = mwp_sys_button; t->window_frame = mwp_sys_window_frame;
    t->log = mwp_sys_log; t->rand32 = mwp_sys_rand32;
    t->args = ""; t->clipboard_get = mwp_sys_clip_get; t->clipboard_set = mwp_sys_clip_set;
    t->set_size = mwp_sys_set_size; t->set_icon = mwp_sys_set_icon; t->theme_color = mwp_sys_theme_color; t->beep = mwp_sys_beep;
    mwp_on_start = mwp_api2_start; mwp_on_exit = mwp_api2_release;
    cpu_fault_hook = pw_fault;            /* a CPU exception inside a program in a window stops that program only (apps/progwin.h) */
    mwp_mx = VGA_WIDTH / 2; mwp_my = VGA_HEIGHT / 2;
}

#endif
