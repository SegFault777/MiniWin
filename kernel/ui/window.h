#ifndef MW_UI_WINDOW_H
#define MW_UI_WINDOW_H

/* ui/window.h -- window_t, the five window slots, z-order, geometry, maximize/restore.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Window state + layout
 *
 * Position/size are runtime fields (not compile-time constants) so the
 * window can be dragged by its title bar and maximized/restored. All
 * drawing and hit-testing below reads from `notepad.x/y/w/h` rather than
 * fixed macros.
 * ============================================================ */
#define WIN_DEFAULT_X   24
#define WIN_DEFAULT_Y   14
#define WIN_DEFAULT_W   320
#define WIN_DEFAULT_H   170
#define MIN_WIN_H       (TITLEBAR_H + FONT_CELL + 6 + 30) /* title + menu + a little edit area */
#define MIN_WIN_W       170                     /* enough for the 3 title bar buttons + a sliver of title text */

/* Maximized geometry: fill the screen above the taskbar entirely. */
#define MAXIMIZED_X 0
#define MAXIMIZED_Y 0
#define MAXIMIZED_W VGA_WIDTH
#define MAXIMIZED_H (VGA_HEIGHT - TASKBAR_H)

/* title bar control buttons: _  []  X, right-aligned, sized to stay
 * comfortably clickable now that everything else on screen grew with
 * the 11px font -- a fat-fingered click target matters more than ever
 * once the whole UI isn't hugging 8px-grid coordinates anymore. */
/* (TITLEBAR_H, BTN_W, BTN_H, BTN_GAP now come from ui/theme.h) */

typedef struct {
    int open;         /* window exists at all (opened from desktop icon) */
    int minimized;    /* currently minimized to the taskbar */
    int maximized;    /* currently maximized to fill the screen above the taskbar */

    /* Current on-screen geometry (the authoritative rect used for drawing
     * and hit-testing whenever the window is visible). While maximized,
     * this is kept equal to the MAXIMIZED_* rect; restoring copies
     * restore_x/y/w/h back into x/y/w/h. */
    int x, y, w, h;

    /* Geometry to snap back to when un-maximizing. */
    int restore_x, restore_y, restore_w, restore_h;
} window_t;

/* SETTING.MWP's window state lives here so the taskbar (which needs to
 * know every window's minimized state) can see it without forward-
 * declaration games. The geometry constants, hit-tests, and drawing code
 * stay grouped with the rest of SETTING.MWP further down. */
#define SETTING_DEFAULT_X   100
#define SETTING_DEFAULT_Y   40
#define SETTING_DEFAULT_W   260
#define SETTING_DEFAULT_H   160

static window_t setting = {
    .open = 0, .minimized = 0, .maximized = 0,
    .x = SETTING_DEFAULT_X, .y = SETTING_DEFAULT_Y,
    .w = SETTING_DEFAULT_W, .h = SETTING_DEFAULT_H,
    .restore_x = SETTING_DEFAULT_X, .restore_y = SETTING_DEFAULT_Y,
    .restore_w = SETTING_DEFAULT_W, .restore_h = SETTING_DEFAULT_H,
};

/* WEB.MWP's window state, same treatment as setting's above -- a
 * single-instance window_t living here so the taskbar/z-order/generic
 * resize machinery can see it, with the app-specific drawing and hit-
 * testing code grouped further down near draw_web_window(). Tall enough
 * to comfortably fit the URL bar row above the response viewport. */
#define WEB_DEFAULT_X   170
#define WEB_DEFAULT_Y   60
#define WEB_DEFAULT_W   340
#define WEB_DEFAULT_H   210

static window_t web_win = {
    .open = 0, .minimized = 0, .maximized = 0,
    .x = WEB_DEFAULT_X, .y = WEB_DEFAULT_Y,
    .w = WEB_DEFAULT_W, .h = WEB_DEFAULT_H,
    .restore_x = WEB_DEFAULT_X, .restore_y = WEB_DEFAULT_Y,
    .restore_w = WEB_DEFAULT_W, .restore_h = WEB_DEFAULT_H,
};

/* Terminal.mwp's window state, same treatment as setting's/web_win's
 * above. Wide enough for ~40 columns of 11px text plus margins -- see
 * this file's Terminal section (further down, past MiniWeb) for the
 * scrollback buffer, command parser, and the actual command table. */
#define TERM_DEFAULT_X   80
#define TERM_DEFAULT_Y   90
#define TERM_DEFAULT_W   452
#define TERM_DEFAULT_H   230

static window_t term_win = {
    .open = 0, .minimized = 0, .maximized = 0,
    .x = TERM_DEFAULT_X, .y = TERM_DEFAULT_Y,
    .w = TERM_DEFAULT_W, .h = TERM_DEFAULT_H,
    .restore_x = TERM_DEFAULT_X, .restore_y = TERM_DEFAULT_Y,
    .restore_w = TERM_DEFAULT_W, .restore_h = TERM_DEFAULT_H,
};

/* ---- title-bar button geometry, shared by every window ----
 * The three buttons (_ [] X) hug the window's top-right corner. `inset_x` is the gap between the window's right
 * edge and the close button, `inset_y` the gap below the top edge: 2 and 1 for Notepad / Web / Terminal; the
 * Setting window has always used 3 and 2 (its frame is a pixel thicker on that side) and keeps doing so. Drawing
 * lives in ui/widgets.h (ui_titlebar_buttons), which takes these positions as plain numbers. */
static inline int win_btn_close_x(const window_t *w, int inset_x) { return w->x + w->w - inset_x - BTN_W; }
static inline int win_btn_max_x(const window_t *w, int inset_x)   { return win_btn_close_x(w, inset_x) - BTN_W - BTN_GAP; }
static inline int win_btn_min_x(const window_t *w, int inset_x)   { return win_btn_max_x(w, inset_x) - BTN_W - BTN_GAP; }
static inline int win_btn_y(const window_t *w, int inset_y)       { return w->y + inset_y; }

#endif
