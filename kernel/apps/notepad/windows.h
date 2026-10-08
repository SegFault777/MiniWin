#ifndef MW_APPS_NOTEPAD_WINDOWS_H
#define MW_APPS_NOTEPAD_WINDOWS_H

/* apps/notepad/windows.h -- the NOTEPAD_MAX Notepad windows: state, creation, focus, per-window text buffers.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Multiple Notepad windows
 *
 * Up to NOTEPAD_MAX independent Notepad windows can be open at once
 * (matches FS_MAX_FILES -- also just a sane cap for how many overlapping
 * windows make sense on a 320x200 screen). Each one carries its own
 * geometry, its own document buffer, its own bound-file slot, its own
 * File-menu-open flag, and its own Save-changes confirm dialog state --
 * none of that is shared between windows anymore.
 *
 * Every Notepad-specific helper function below (draw_window(),
 * btn_min_x(), file_label_hit(), save_current_document(), ...) reads and
 * writes through a single scratch pointer, `active_np`, rather than
 * taking a notepad_t* parameter directly. Whichever bit of code is about
 * to draw, click-test, or type into a particular window sets active_np
 * first. This is a deliberately simple "current context" pattern instead
 * of threading a pointer through a couple dozen function signatures --
 * there's only ever one Notepad window being drawn, clicked, or typed
 * into at any given instant anyway, even though up to four can exist.
 * ============================================================ */
#define NOTEPAD_MAX 4

typedef struct {
    window_t win;
    int id;                          /* index into notepads[] -- set once at boot */
    char text_buf[FS_MAX_FILE_BYTES];
    u32  text_len;
    int  bound_slot;                 /* which FS slot this document is saved to, -1 = unbound */
    int  file_menu_open;
    int  confirm_mode;               /* CONFIRM_NONE / CONFIRM_NEW / CONFIRM_CLOSE, this window's own */
} notepad_t;

static notepad_t notepads[NOTEPAD_MAX];
static notepad_t *active_np = &notepads[0];

/* ------------------------------------------------------------
 * Window IDs + z-order (stacking) + minimize-order (taskbar layout)
 *
 * A "window id" is just 0..NOTEPAD_MAX-1 for notepads[id], or
 * WIN_ID_SETTING for the Setting window, WIN_ID_WEB for the MiniWeb
 * browser window, or WIN_ID_TERMINAL for Terminal.mwp -- one small
 * integer namespace covering every top-level window in the OS, so the
 * taskbar, z-order, and focus-on-click logic can all treat "which
 * window" generically instead of hardcoding every app by name at every
 * call site.
 * ------------------------------------------------------------ */
#define WIN_ID_SETTING  NOTEPAD_MAX
#define WIN_ID_WEB      (NOTEPAD_MAX + 1)
#define WIN_ID_TERMINAL (NOTEPAD_MAX + 2)
#define WIN_ID_COUNT    (NOTEPAD_MAX + 3)

static int win_is_open(int id) {
    if (id == WIN_ID_SETTING) return setting.open;
    if (id == WIN_ID_WEB) return web_win.open;
    if (id == WIN_ID_TERMINAL) return term_win.open;
    return notepads[id].win.open;
}
static int win_is_minimized(int id) {
    if (id == WIN_ID_SETTING) return setting.minimized;
    if (id == WIN_ID_WEB) return web_win.minimized;
    if (id == WIN_ID_TERMINAL) return term_win.minimized;
    return notepads[id].win.minimized;
}
static window_t *win_ptr(int id) {
    if (id == WIN_ID_SETTING) return &setting;
    if (id == WIN_ID_WEB) return &web_win;
    if (id == WIN_ID_TERMINAL) return &term_win;
    return &notepads[id].win;
}

/* z_order[0..z_count-1] lists every currently-OPEN window id, back
 * (bottom) to front (top). Closing a window removes it; clicking one
 * (or opening/restoring it) moves it to the end, i.e. the front -- the
 * entire "clicking a window brings it to the front" feature is just
 * these three tiny functions plus render_frame() drawing in this order
 * and the click-handler hit-testing in reverse. */
static int z_order[WIN_ID_COUNT];
static int z_count = 0;

static void win_z_remove(int id) {
    for (int i = 0; i < z_count; i++) {
        if (z_order[i] == id) {
            for (int j = i; j < z_count - 1; j++) z_order[j] = z_order[j + 1];
            z_count--;
            return;
        }
    }
}
static void win_z_raise(int id) {
    win_z_remove(id);
    z_order[z_count++] = id;
}

/* ------------------------------------------------------------
 * Title-bar button press state -- shared by every _/[]/X button in the
 * OS (Notepad's, Setting's, and the Warning dialog's lone X). Pressing
 * one down doesn't fire it immediately anymore: it just "arms" that
 * specific button (kind + which window) and the button's own bevel
 * flips to a sunken look. The action only actually happens when the
 * mouse button is RELEASED while still over that same button -- the
 * same press-hold-release contract every real button widget uses, and
 * releasing anywhere else (drag off first) quietly cancels it.
 * ------------------------------------------------------------ */
#define BTN_NONE          0
#define BTN_MIN           1
#define BTN_MAX           2
#define BTN_CLOSE         3
#define BTN_CONFIRM_CLOSE 4  /* the Warning dialog's X -- tied to a notepad id like BTN_CLOSE is */
#define BTN_WEB_GO        5  /* the URL bar's GO button -- always WIN_ID_WEB,
                              * but tracked the same press-hold-release
                              * way as every other button here so a drag-
                              * off cancels it instead of firing early */

static int pressed_btn_kind = BTN_NONE;
static int pressed_btn_win = -1;   /* which window id this press belongs to (WIN_ID_SETTING or a notepad index) */

/* (draw_bevel_button() lives in ui/widgets.h now, as ui_bevel().) */

/* Separate from z-order on purpose: z-order is about on-screen stacking
 * of VISIBLE windows, while this is purely "what order were things
 * minimized in," used only to lay out taskbar pills left-to-right in
 * that order. A window keeps its z-order slot while minimized (so
 * restoring puts it back where clicking-to-focus would), but it has no
 * on-screen rect to stack while minimized, hence the separate concept. */
static u32 minimize_seq[WIN_ID_COUNT];
static u32 next_minimize_seq = 1;
static void win_minimize(int id) {
    window_t *w_ = win_ptr(id);
    w_->minimized = 1;
    minimize_seq[id] = next_minimize_seq++;
}
static void win_restore(int id) {
    win_ptr(id)->minimized = 0;
    win_z_raise(id); /* restoring a window also focuses it, like real WMs do */
}

/* Actual body of ime_cycle_next(), forward-declared above -- now that
 * notepad_t exists, it can flush a syllable mid-switch into whichever
 * window currently has keyboard focus. */
static void ime_cycle_next(void) {
    for (int step = 1; step <= IME_COUNT; step++) {
        int candidate = (current_ime + step) % IME_COUNT;
        if (ime_enabled[candidate]) {
            if (candidate != current_ime && ko_ime_is_composing() && active_np) {
                ko_ime_commit(active_np->text_buf, &active_np->text_len, sizeof(active_np->text_buf));
            }
            current_ime = candidate;
            return;
        }
    }
}

#endif
