#ifndef MW_APPS_NOTEPAD_FILE_MENU_H
#define MW_APPS_NOTEPAD_FILE_MENU_H

/* apps/notepad/file_menu.h -- Notepad's File menu: items, geometry, hit-testing.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * File menu (dropdown from the "File" label in the menu bar)
 * and the Yes/No confirm dialog used by "New" and the close (X) button.
 * (active_np->file_menu_open and active_np->confirm_mode are per-window now -- see notepad_t --
 * so there's nothing to declare here anymore, just the CONFIRM_* values
 * and the Start Menu flag, which really are global.)
 * ============================================================ */

/* Whether the Windows-95-style Start Menu is currently popped up. Unlike
 * the File dropdown above (which only exists while Notepad's window is
 * open), this one lives entirely in the taskbar and couldn't care less
 * what Notepad is doing. */
static int start_menu_open = 0;

#define CONFIRM_NONE  0
#define CONFIRM_NEW   1  /* "Save changes before New?" */
#define CONFIRM_CLOSE 2  /* "Save changes before closing?" */

/* Desktop file icons: which of the FS_MAX_FILES slots currently hold a
 * saved file (either saved earlier this session, or found already present
 * at boot -- persistence is real, backed by fs.h/ata.h), and their sizes. */
static int desktop_file_exists[FS_MAX_FILES];
static u32 desktop_file_len[FS_MAX_FILES];

/* (active_np->bound_slot is per-window now too -- see notepad_t.active_np->bound_slot -- since
 * each open document binds to its own file slot independently.) */

/* Shared status-line message, file scope so helper functions below (save
 * logic, confirm dialog actions) can set it directly. */
static const char *status = "MINIWIN 1.0 - DOUBLE-CLICK NOTEPAD.MWP TO OPEN";
static char status_buf[48]; /* scratch space for status messages that embed a filename */

#define MENU_FILE_LABEL_W 46   /* clickable width for the "File" label */
#define MENU_ITEM_SPACING 54   /* x-step between File/Edit/Help labels -- a bit
                                 * wider than MENU_FILE_LABEL_W so consecutive
                                 * labels get visible breathing room instead of
                                 * touching edge-to-edge */
#define FILE_MENU_ITEM_H  15
#define FILE_MENU_W       128 /* wide enough for "다른 이름으로 저장" (Save As, Korean, 9 glyphs @ FONT_CELL) */

static inline int menu_y_pos(void) { return active_np->win.y + TITLEBAR_H + 1; }
static inline int file_menu_x(void) { return active_np->win.x + 4; }
static inline int file_menu_top_y(void) { return menu_y_pos() + FONT_CELL + 3; }

static int file_label_hit(int px, int py) {
    return in_rect(px, py, active_np->win.x + 4, menu_y_pos(), MENU_FILE_LABEL_W, FONT_CELL + 3);
}

static int file_menu_item_hit(int px, int py, int idx) {
    /* idx: 0=Save As, 1=Save, 2=New */
    int x = file_menu_x();
    int y = file_menu_top_y() + idx * FILE_MENU_ITEM_H;
    return in_rect(px, py, x, y, FILE_MENU_W, FILE_MENU_ITEM_H);
}

/* Warning dialog: a real title bar now (blue, "Warning!", X only -- no
 * minimize/maximize, same restrained chrome as SETTING.MWP) sitting on
 * top of the message + Yes/No area. Widened a touch to leave room for
 * the warning icon next to the first line of text. */
#define CONFIRM_W 210
#define CONFIRM_H 90
#define CONFIRM_BTN_W 46
#define CONFIRM_BTN_H 17

static inline int confirm_x(void) { return active_np->win.x + (active_np->win.w - CONFIRM_W) / 2; }
static inline int confirm_y(void) { return active_np->win.y + (active_np->win.h - CONFIRM_H) / 2; }
static inline int confirm_btn_y(void) { return confirm_y() + CONFIRM_H - CONFIRM_BTN_H - 11; }
static inline int confirm_yes_x(void) { return confirm_x() + 22; }
static inline int confirm_no_x(void)  { return confirm_x() + CONFIRM_W - 22 - CONFIRM_BTN_W; }
static inline int confirm_close_x(void) { return confirm_x() + CONFIRM_W - 3 - BTN_W; }
static inline int confirm_close_y(void) { return confirm_y() + 1; }

static int confirm_yes_hit(int px, int py) {
    return in_rect(px, py, confirm_yes_x(), confirm_btn_y(), CONFIRM_BTN_W, CONFIRM_BTN_H);
}
static int confirm_no_hit(int px, int py) {
    return in_rect(px, py, confirm_no_x(), confirm_btn_y(), CONFIRM_BTN_W, CONFIRM_BTN_H);
}
/* The title bar's X -- same as clicking neither Yes nor No: bails out
 * of whatever triggered the dialog (New / Close) and leaves the
 * document exactly as it was, untouched and unsaved-but-not-lost. */
static int confirm_close_hit(int px, int py) {
    return in_rect(px, py, confirm_close_x(), confirm_close_y(), BTN_W, BTN_H);
}

#endif
