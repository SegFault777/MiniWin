#include "io.h"
#include "vga.h"
#include "font.h"
#include "keyboard.h"
#include "mouse.h"
#include "ata.h"
#include "fs.h"
#include "font_ko.h"
#include "hangul_ime.h"
#include "speaker.h"
#include "serial.h"
#include "pci.h"

#define DESKTOP_COLOR_BG      COL_LCYAN
#define DESKTOP_COLOR_ICON_BG COL_LCYAN

/* ---------- tiny freestanding helpers (no libc available) ---------- */
static void kstrcpy_append(char *buf, u32 *len, u32 maxlen, char c) {
    if (*len < maxlen - 1) {
        buf[*len] = c;
        (*len)++;
        buf[*len] = 0;
    }
}

/* simple busy-wait delay, calibrated roughly for typical QEMU/CPU speed */
static void delay(volatile u32 loops) {
    while (loops--) { __asm__ volatile ("nop"); }
}

static inline int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static inline int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ============================================================
 * System language & IME (SETTING.EXE > SYSTEM)
 *
 * Two genuinely separate settings, even though today they both happen
 * to be a choice of exactly the same two languages:
 *   - sys_language: what language the OS CHROME (menus, dialogs, status
 *     bar) is drawn in. Doesn't affect what you can type.
 *   - ime_enabled[] / current_ime: which input method(s) Right Alt is
 *     allowed to cycle through while typing in Notepad. Doesn't affect
 *     what language the menus are in.
 * Mixing them up would mean you couldn't type English in a Korean-
 * language system or vice versa, which is exactly the annoying
 * limitation real operating systems learned not to have decades ago.
 * ============================================================ */
#define LANG_ENGLISH 0
#define LANG_KOREAN  1
static int sys_language = LANG_ENGLISH;

#define IME_ENGLISH 0
#define IME_KOREAN  1
#define IME_COUNT   2
/* Both on by default -- matches the old F7/Right-Alt behavior, which
 * could always reach either language with no setup required. */
static int ime_enabled[IME_COUNT] = { 1, 1 };
static int current_ime = IME_ENGLISH;

/* Right Alt cycles to the next ENABLED ime, in fixed order, wrapping
 * around. If only one box is checked in SETTING.EXE, the loop below
 * walks all the way around back to the one we started on and quietly
 * changes nothing -- same as real Windows with a single input method
 * installed: the key is still there, it just has nowhere to go.
 * (Defined further down, right after text_buf/text_len exist -- it
 * needs to flush a syllable into that buffer mid-switch.) */
static void ime_cycle_next(void);

/* Called right after SETTING.EXE flips one of the IME checkboxes. If
 * the IME you were actively using just got unchecked, bump over to
 * whichever one is still enabled instead of leaving current_ime
 * pointing at a now-disabled option nobody can reach. */
static void ime_ensure_current_enabled(void) {
    if (ime_enabled[current_ime]) return;
    for (int i = 0; i < IME_COUNT; i++) {
        if (ime_enabled[i]) { current_ime = i; return; }
    }
}

/* ------------------------------------------------------------
 * UI string table. Every bit of chrome text that isn't a literal
 * filename (NOTEPAD.EXE stays NOTEPAD.EXE in any language, same as it
 * would on real Windows) goes through here, so SETTING.EXE's Language
 * switch actually means something. Language *names* themselves
 * ("English", "한국어") are deliberately NOT in this table -- every
 * real language picker shows each language's own name in its own
 * script so you can find yours even if you can't currently read
 * whatever's selected.
 * ------------------------------------------------------------ */
typedef enum {
    STR_SYSTEM, STR_LANGUAGE, STR_IME,
    STR_FILE, STR_EDIT, STR_HELP,
    STR_SAVE_AS, STR_SAVE, STR_NEW,
    STR_YES, STR_NO,
    STR_SAVE_CHANGES, STR_BEFORE_CLOSING, STR_BEFORE_NEW,
    STR_SHUT_DOWN, STR_RESTART, STR_SAFE_TO_TURN_OFF,
    STR_DEFAULT_HINT,
    STR_NOTEPAD_OPENED, STR_SETTING_OPENED,
    STR_NOTEPAD_MINIMIZED, STR_NOTEPAD_MAXIMIZED, STR_NOTEPAD_RESTORED,
    STR_SETTING_MINIMIZED, STR_SETTING_MAXIMIZED, STR_SETTING_RESTORED,
    STR_SAVE_AS_COMING_SOON,
    STR_STORAGE_FULL_NOT_SAVED,
    STR_SAVED_PREFIX,
    STR_SAVED_AND_CLOSED, STR_STORAGE_FULL_CLOSED_NOT_SAVED,
    STR_NEW_DOCUMENT_NOT_SAVED, STR_CLOSED_NOT_SAVED,
    STR_HANGUL_MODE_ON, STR_ENGLISH_MODE_ON,
    STR_IME_MIN_ONE,
    STR_COUNT
} ui_str_id;

static const char *ui_strings_en[STR_COUNT] = {
    [STR_SYSTEM] = "SYSTEM",
    [STR_LANGUAGE] = "Language",
    [STR_IME] = "IME",
    [STR_FILE] = "File",
    [STR_EDIT] = "Edit",
    [STR_HELP] = "Help",
    [STR_SAVE_AS] = "Save As",
    [STR_SAVE] = "Save",
    [STR_NEW] = "New",
    [STR_YES] = "Yes",
    [STR_NO] = "No",
    [STR_SAVE_CHANGES] = "Save changes",
    [STR_BEFORE_CLOSING] = "before closing?",
    [STR_BEFORE_NEW] = "before New?",
    [STR_SHUT_DOWN] = "Shut Down",
    [STR_RESTART] = "Restart",
    [STR_SAFE_TO_TURN_OFF] = "It's now safe to turn off your computer.",
    [STR_DEFAULT_HINT] = "MINIWIN 1.0 - DOUBLE-CLICK NOTEPAD.EXE TO OPEN",
    [STR_NOTEPAD_OPENED] = "NOTEPAD.EXE OPENED (RIGHT ALT: SWITCH IME)",
    [STR_SETTING_OPENED] = "SETTING.EXE OPENED",
    [STR_NOTEPAD_MINIMIZED] = "NOTEPAD.EXE MINIMIZED",
    [STR_NOTEPAD_MAXIMIZED] = "NOTEPAD.EXE MAXIMIZED",
    [STR_NOTEPAD_RESTORED] = "NOTEPAD.EXE RESTORED",
    [STR_SETTING_MINIMIZED] = "SETTING.EXE MINIMIZED",
    [STR_SETTING_MAXIMIZED] = "SETTING.EXE MAXIMIZED",
    [STR_SETTING_RESTORED] = "SETTING.EXE RESTORED",
    [STR_SAVE_AS_COMING_SOON] = "SAVE AS - COMING SOON",
    [STR_STORAGE_FULL_NOT_SAVED] = "STORAGE FULL - NOT SAVED",
    [STR_SAVED_PREFIX] = "SAVED: ",
    [STR_SAVED_AND_CLOSED] = "SAVED AND CLOSED",
    [STR_STORAGE_FULL_CLOSED_NOT_SAVED] = "STORAGE FULL - CLOSED, NOT SAVED",
    [STR_NEW_DOCUMENT_NOT_SAVED] = "NEW DOCUMENT (NOT SAVED)",
    [STR_CLOSED_NOT_SAVED] = "CLOSED (NOT SAVED)",
    [STR_HANGUL_MODE_ON] = "HANGUL MODE ON (RIGHT ALT TO SWITCH)",
    [STR_ENGLISH_MODE_ON] = "ENGLISH MODE ON (RIGHT ALT TO SWITCH)",
    [STR_IME_MIN_ONE] = "AT LEAST ONE IME MUST STAY ENABLED",
};

static const char *ui_strings_ko[STR_COUNT] = {
    [STR_SYSTEM] = "\xec\x8b\x9c\xec\x8a\xa4\xed\x85\x9c",
    [STR_LANGUAGE] = "\xec\x96\xb8\xec\x96\xb4",
    [STR_IME] = "\xec\x9e\x85\xeb\xa0\xa5\xea\xb8\xb0",
    [STR_FILE] = "\xed\x8c\x8c\xec\x9d\xbc",
    [STR_EDIT] = "\xed\x8e\xb8\xec\xa7\x91",
    [STR_HELP] = "\xeb\x8f\x84\xec\x9b\x80\xeb\xa7\x90",
    [STR_SAVE_AS] = "\xeb\x8b\xa4\xeb\xa5\xb8 \xec\x9d\xb4\xeb\xa6\x84\xec\x9c\xbc\xeb\xa1\x9c \xec\xa0\x80\xec\x9e\xa5",
    [STR_SAVE] = "\xec\xa0\x80\xec\x9e\xa5",
    [STR_NEW] = "\xec\x83\x88\xeb\xa1\x9c \xeb\xa7\x8c\xeb\x93\xa4\xea\xb8\xb0",
    [STR_YES] = "\xec\x98\x88",
    [STR_NO] = "\xec\x95\x84\xeb\x8b\x88\xec\x98\xa4",
    [STR_SAVE_CHANGES] = "\xec\xa0\x80\xec\x9e\xa5\xed\x95\x98\xec\x8b\x9c\xea\xb2\xa0\xec\x8a\xb5\xeb\x8b\x88\xea\xb9\x8c,",
    [STR_BEFORE_CLOSING] = "\xeb\x8b\xab\xea\xb8\xb0 \xec\xa0\x84\xec\x97\x90?",
    [STR_BEFORE_NEW] = "\xec\x83\x88\xeb\xa1\x9c \xeb\xa7\x8c\xeb\x93\xa4\xea\xb8\xb0 \xec\xa0\x84\xec\x97\x90?",
    [STR_SHUT_DOWN] = "\xec\x8b\x9c\xec\x8a\xa4\xed\x85\x9c \xec\xa2\x85\xeb\xa3\x8c",
    [STR_RESTART] = "\xeb\x8b\xa4\xec\x8b\x9c \xec\x8b\x9c\xec\x9e\x91",
    [STR_SAFE_TO_TURN_OFF] = "\xec\x9d\xb4\xec\xa0\x9c \xec\xbb\xb4\xed\x93\xa8\xed\x84\xb0\xeb\xa5\xbc \xea\xba\xbc\xeb\x8f\x84 \xeb\x90\xa9\xeb\x8b\x88\xeb\x8b\xa4.",
    [STR_DEFAULT_HINT] = "MINIWIN 1.0 - NOTEPAD.EXE \xeb\x8d\x94\xeb\xb8\x94\xed\x81\xb4\xeb\xa6\xad\xec\x9c\xbc\xeb\xa1\x9c \xec\x8b\xa4\xed\x96\x89",
    [STR_NOTEPAD_OPENED] = "NOTEPAD.EXE \xec\x8b\xa4\xed\x96\x89\xeb\x90\xa8 (RIGHT ALT: \xec\x9e\x85\xeb\xa0\xa5\xea\xb8\xb0 \xec\xa0\x84\xed\x99\x98)",
    [STR_SETTING_OPENED] = "SETTING.EXE \xec\x8b\xa4\xed\x96\x89\xeb\x90\xa8",
    [STR_NOTEPAD_MINIMIZED] = "NOTEPAD.EXE \xec\xb5\x9c\xec\x86\x8c\xed\x99\x94\xeb\x90\xa8",
    [STR_NOTEPAD_MAXIMIZED] = "NOTEPAD.EXE \xec\xb5\x9c\xeb\x8c\x80\xed\x99\x94\xeb\x90\xa8",
    [STR_NOTEPAD_RESTORED] = "NOTEPAD.EXE \xeb\xb3\xb5\xec\x9b\x90\xeb\x90\xa8",
    [STR_SETTING_MINIMIZED] = "SETTING.EXE \xec\xb5\x9c\xec\x86\x8c\xed\x99\x94\xeb\x90\xa8",
    [STR_SETTING_MAXIMIZED] = "SETTING.EXE \xec\xb5\x9c\xeb\x8c\x80\xed\x99\x94\xeb\x90\xa8",
    [STR_SETTING_RESTORED] = "SETTING.EXE \xeb\xb3\xb5\xec\x9b\x90\xeb\x90\xa8",
    [STR_SAVE_AS_COMING_SOON] = "\xeb\x8b\xa4\xeb\xa5\xb8 \xec\x9d\xb4\xeb\xa6\x84\xec\x9c\xbc\xeb\xa1\x9c \xec\xa0\x80\xec\x9e\xa5 - \xec\xa4\x80\xeb\xb9\x84 \xec\xa4\x91",
    [STR_STORAGE_FULL_NOT_SAVED] = "\xec\xa0\x80\xec\x9e\xa5 \xea\xb3\xb5\xea\xb0\x84 \xeb\xb6\x80\xec\xa1\xb1 - \xec\xa0\x80\xec\x9e\xa5 \xec\x95\x88 \xeb\x90\xa8",
    [STR_SAVED_PREFIX] = "\xec\xa0\x80\xec\x9e\xa5\xeb\x90\xa8: ",
    [STR_SAVED_AND_CLOSED] = "\xec\xa0\x80\xec\x9e\xa5 \xed\x9b\x84 \xeb\x8b\xab\xec\x9d\x8c",
    [STR_STORAGE_FULL_CLOSED_NOT_SAVED] = "\xec\xa0\x80\xec\x9e\xa5 \xea\xb3\xb5\xea\xb0\x84 \xeb\xb6\x80\xec\xa1\xb1 - \xec\xa0\x80\xec\x9e\xa5 \xec\x95\x88 \xed\x95\x98\xea\xb3\xa0 \xeb\x8b\xab\xec\x9d\x8c",
    [STR_NEW_DOCUMENT_NOT_SAVED] = "\xec\x83\x88 \xeb\xac\xb8\xec\x84\x9c (\xec\xa0\x80\xec\x9e\xa5 \xec\x95\x88 \xeb\x90\xa8)",
    [STR_CLOSED_NOT_SAVED] = "\xeb\x8b\xab\xec\x9d\x8c (\xec\xa0\x80\xec\x9e\xa5 \xec\x95\x88 \xeb\x90\xa8)",
    [STR_HANGUL_MODE_ON] = "\xed\x95\x9c\xea\xb8\x80 \xeb\xaa\xa8\xeb\x93\x9c \xec\xbc\x9c\xec\xa7\x90 (RIGHT ALT\xeb\xa1\x9c \xec\xa0\x84\xed\x99\x98)",
    [STR_ENGLISH_MODE_ON] = "\xec\x98\x81\xec\x96\xb4 \xeb\xaa\xa8\xeb\x93\x9c \xec\xbc\x9c\xec\xa7\x90 (RIGHT ALT\xeb\xa1\x9c \xec\xa0\x84\xed\x99\x98)",
    [STR_IME_MIN_ONE] = "\xec\xb5\x9c\xec\x86\x8c 1\xea\xb0\x9c\xec\x9d\x98 \xec\x9e\x85\xeb\xa0\xa5\xea\xb8\xb0\xeb\x8a\x94 \xec\xbc\x9c\xec\xa0\xb8 \xec\x9e\x88\xec\x96\xb4\xec\x95\xbc \xed\x95\xa8",
};


static inline const char *t(ui_str_id id) {
    return (sys_language == LANG_KOREAN) ? ui_strings_ko[id] : ui_strings_en[id];
}

/* ============================================================
 * Desktop icon
 * ============================================================ */
/* Both desktop icons sit in equal-width "slots" -- the icon glyph and
 * each line of its label are centered within the slot independently,
 * so a wide label (like "NOTEPAD", wider than the 16px icon above it)
 * overflows the same amount on both sides instead of trailing off to
 * one side the way raw left-aligned coordinates used to. */
#define ICON_SLOT_W 60
#define ICON_GLYPH_W 16

#define ICON_X   4
#define ICON_Y   6
#define ICON_W   ICON_SLOT_W
#define ICON_H   26   /* box + label combined hit area */

static void draw_desktop_icon(void) {
    int gx = ICON_X + (ICON_SLOT_W - ICON_GLYPH_W) / 2;
    bb_fillrect(gx, ICON_Y, ICON_GLYPH_W, 12, COL_WHITE);
    bb_rect(gx, ICON_Y, ICON_GLYPH_W, 12, COL_BLACK);
    /* little folded-corner notch to look like a document/app icon */
    bb_fillrect(gx + 11, ICON_Y, 5, 4, DESKTOP_COLOR_ICON_BG);
    bb_rect(gx + 11, ICON_Y, 5, 4, COL_BLACK);

    const char *line1 = "NOTEPAD", *line2 = ".EXE";
    font_draw_string(ICON_X + (ICON_SLOT_W - 8 * 7) / 2, ICON_Y + 14, line1, COL_BLACK);
    font_draw_string(ICON_X + (ICON_SLOT_W - 8 * 4) / 2, ICON_Y + 22, line2, COL_BLACK);
}

/* SETTING.EXE -- sits next to NOTEPAD.EXE in the same top row.
 * Double-clicking opens the real SYSTEM settings window (Language, IME)
 * defined further down, right alongside Notepad's own window code. */
#define ICON2_X  (ICON_X + ICON_SLOT_W + 8)
#define ICON2_Y  6
#define ICON2_W  ICON_SLOT_W
#define ICON2_H  26

static void draw_desktop_icon2(void) {
    /* simple gear-ish glyph so it reads as a distinct app, not another
     * document -- a filled circle-ish square with a few notches */
    int gx = ICON2_X + (ICON_SLOT_W - ICON_GLYPH_W) / 2;
    bb_fillrect(gx, ICON2_Y, ICON_GLYPH_W, 12, COL_LGRAY);
    bb_rect(gx, ICON2_Y, ICON_GLYPH_W, 12, COL_BLACK);
    bb_fillrect(gx + 4, ICON2_Y + 3, 8, 6, COL_WHITE);
    bb_rect(gx + 4, ICON2_Y + 3, 8, 6, COL_BLACK);
    bb_putpixel(gx + 2, ICON2_Y + 1, COL_BLACK);
    bb_putpixel(gx + 13, ICON2_Y + 1, COL_BLACK);
    bb_putpixel(gx + 2, ICON2_Y + 10, COL_BLACK);
    bb_putpixel(gx + 13, ICON2_Y + 10, COL_BLACK);

    const char *line1 = "SETTING", *line2 = ".EXE";
    font_draw_string(ICON2_X + (ICON_SLOT_W - 8 * 7) / 2, ICON2_Y + 14, line1, COL_BLACK);
    font_draw_string(ICON2_X + (ICON_SLOT_W - 8 * 4) / 2, ICON2_Y + 22, line2, COL_BLACK);
}

/* ============================================================
 * Taskbar (bottom of screen, Windows-95-ish strip)
 * ============================================================ */
#define TASKBAR_H     14
#define TASKBAR_Y     (VGA_HEIGHT - TASKBAR_H)

/* The Start button -- bottom-left corner, obviously. Every desktop OS
 * since 1995 has agreed on this location without ever holding a
 * meeting about it. Sized to fit "AM" plus a proper raised bevel. */
#define STARTBTN_X    2
#define STARTBTN_Y    (TASKBAR_Y + 2)
#define STARTBTN_W    24
#define STARTBTN_H    10

/* The minimized-Notepad pill used to hug the taskbar's left edge; now
 * it scoots over to make room for its new neighbor. */
#define TASKBTN_X     (STARTBTN_X + STARTBTN_W + 4)
#define TASKBTN_Y     (TASKBAR_Y + 2)
#define TASKBTN_W     70
#define TASKBTN_H     10

/* SETTING.EXE gets its own taskbar slot right next to Notepad's, since
 * both windows can now be independently minimized at the same time. */
#define TASKBTN2_X    (TASKBTN_X + TASKBTN_W + 4)
#define TASKBTN2_Y    TASKBTN_Y
#define TASKBTN2_W    TASKBTN_W
#define TASKBTN2_H    TASKBTN_H

/* ============================================================
 * Window state + layout
 *
 * Position/size are runtime fields (not compile-time constants) so the
 * window can be dragged by its title bar and maximized/restored. All
 * drawing and hit-testing below reads from `notepad.x/y/w/h` rather than
 * fixed macros.
 * ============================================================ */
#define WIN_DEFAULT_X   40
#define WIN_DEFAULT_Y   25
#define WIN_DEFAULT_W   280
#define WIN_DEFAULT_H   140
#define TITLEBAR_H      10
#define MIN_WIN_H       (TITLEBAR_H + 12 + 20) /* title + menu + a little edit area */

/* Maximized geometry: fill the screen above the taskbar entirely. */
#define MAXIMIZED_X 0
#define MAXIMIZED_Y 0
#define MAXIMIZED_W VGA_WIDTH
#define MAXIMIZED_H (VGA_HEIGHT - TASKBAR_H)

/* title bar control buttons: _  []  X, right-aligned, 9x8 each */
#define BTN_W 9
#define BTN_H 8
#define BTN_GAP 1

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

static window_t notepad = {
    .open = 0, .minimized = 0, .maximized = 0,
    .x = WIN_DEFAULT_X, .y = WIN_DEFAULT_Y, .w = WIN_DEFAULT_W, .h = WIN_DEFAULT_H,
    .restore_x = WIN_DEFAULT_X, .restore_y = WIN_DEFAULT_Y,
    .restore_w = WIN_DEFAULT_W, .restore_h = WIN_DEFAULT_H,
};

/* SETTING.EXE's window state lives here too (rather than down by the
 * rest of its drawing code) so the taskbar -- which needs to know about
 * both windows' minimized state -- can see it without forward-
 * declaration games. The geometry constants, hit-tests, and drawing
 * code stay grouped with the rest of SETTING.EXE further down. */
#define SETTING_DEFAULT_X   80
#define SETTING_DEFAULT_Y   30
#define SETTING_DEFAULT_W   200
#define SETTING_DEFAULT_H   124

static window_t setting = {
    .open = 0, .minimized = 0, .maximized = 0,
    .x = SETTING_DEFAULT_X, .y = SETTING_DEFAULT_Y,
    .w = SETTING_DEFAULT_W, .h = SETTING_DEFAULT_H,
    .restore_x = SETTING_DEFAULT_X, .restore_y = SETTING_DEFAULT_Y,
    .restore_w = SETTING_DEFAULT_W, .restore_h = SETTING_DEFAULT_H,
};

static char text_buf[FS_MAX_FILE_BYTES];
static u32  text_len = 0;

/* Actual body of ime_cycle_next(), forward-declared above -- now that
 * text_buf/text_len exist, it can flush a syllable mid-switch. */
static void ime_cycle_next(void) {
    for (int step = 1; step <= IME_COUNT; step++) {
        int candidate = (current_ime + step) % IME_COUNT;
        if (ime_enabled[candidate]) {
            if (candidate != current_ime && ko_ime_is_composing()) {
                ko_ime_commit(text_buf, &text_len, sizeof(text_buf));
            }
            current_ime = candidate;
            return;
        }
    }
}

/* ============================================================
 * File menu (dropdown from the "File" label in the menu bar)
 * and the Yes/No confirm dialog used by "New" and the close (X) button.
 * ============================================================ */
static int file_menu_open = 0;

/* Whether the Windows-95-style Start Menu is currently popped up. Unlike
 * the File dropdown above (which only exists while Notepad's window is
 * open), this one lives entirely in the taskbar and couldn't care less
 * what Notepad is doing. */
static int start_menu_open = 0;

#define CONFIRM_NONE  0
#define CONFIRM_NEW   1  /* "Save changes before New?" */
#define CONFIRM_CLOSE 2  /* "Save changes before closing?" */
static int confirm_mode = CONFIRM_NONE;

/* Desktop file icons: which of the FS_MAX_FILES slots currently hold a
 * saved file (either saved earlier this session, or found already present
 * at boot -- persistence is real, backed by fs.h/ata.h), and their sizes. */
static int desktop_file_exists[FS_MAX_FILES];
static u32 desktop_file_len[FS_MAX_FILES];

/* Which slot the currently-open document is "bound" to: -1 means the
 * document is new/unbound (never saved, or created via "New"), so the
 * next Save picks the first empty slot and binds to it. Once bound,
 * subsequent saves update that same slot in place, like a normal editor's
 * "Save" (not creating a new file every time you press Ctrl+S). */
static int bound_slot = -1;

/* Shared status-line message, file scope so helper functions below (save
 * logic, confirm dialog actions) can set it directly. */
static const char *status = "MINIWIN 1.0 - DOUBLE-CLICK NOTEPAD.EXE TO OPEN";
static char status_buf[48]; /* scratch space for status messages that embed a filename */

#define MENU_FILE_LABEL_W 34   /* clickable width for the "File" label */
#define FILE_MENU_ITEM_H  10
#define FILE_MENU_W       94  /* wide enough for "다른 이름으로 저장" (Save As, Korean) */

static inline int menu_y_pos(void) { return notepad.y + TITLEBAR_H + 1; }
static inline int file_menu_x(void) { return notepad.x + 4; }
static inline int file_menu_top_y(void) { return menu_y_pos() + 9; }

static int file_label_hit(int px, int py) {
    return in_rect(px, py, notepad.x + 4, menu_y_pos(), MENU_FILE_LABEL_W, 9);
}

static int file_menu_item_hit(int px, int py, int idx) {
    /* idx: 0=Save As, 1=Save, 2=New */
    int x = file_menu_x();
    int y = file_menu_top_y() + idx * FILE_MENU_ITEM_H;
    return in_rect(px, py, x, y, FILE_MENU_W, FILE_MENU_ITEM_H);
}

/* Warning dialog: a real title bar now (blue, "Warning!", X only -- no
 * minimize/maximize, same restrained chrome as SETTING.EXE) sitting on
 * top of the message + Yes/No area. Widened a touch to leave room for
 * the warning icon next to the first line of text. */
#define CONFIRM_W 160
#define CONFIRM_H 66
#define CONFIRM_BTN_W 34
#define CONFIRM_BTN_H 12

static inline int confirm_x(void) { return notepad.x + (notepad.w - CONFIRM_W) / 2; }
static inline int confirm_y(void) { return notepad.y + (notepad.h - CONFIRM_H) / 2; }
static inline int confirm_btn_y(void) { return confirm_y() + CONFIRM_H - CONFIRM_BTN_H - 8; }
static inline int confirm_yes_x(void) { return confirm_x() + 16; }
static inline int confirm_no_x(void)  { return confirm_x() + CONFIRM_W - 16 - CONFIRM_BTN_W; }
static inline int confirm_close_x(void) { return confirm_x() + CONFIRM_W - 2 - BTN_W; }
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

/* ============================================================
 * Shared save logic -- used by File>Save, Ctrl+S, and the Yes actions of
 * both the "New" and "close window" confirm dialogs, so saving behaves
 * identically no matter which UI path triggered it.
 * ============================================================ */

/* Builds "SAVED: <filename>" (or its Korean equivalent) into status_buf
 * and returns it. */
static const char *format_saved_status(int slot) {
    u32 i = 0;
    const char *prefix = t(STR_SAVED_PREFIX);
    while (*prefix) status_buf[i++] = *prefix++;
    const char *name = fs_slot_names[slot];
    while (*name && i < sizeof(status_buf) - 1) status_buf[i++] = *name++;
    status_buf[i] = 0;
    return status_buf;
}

/* Saves text_buf/text_len to the slot the current document is bound to,
 * or to the first empty slot if unbound (and binds to it, so subsequent
 * saves of the same still-open document update that slot instead of
 * creating a new file each time). Returns the slot saved to, or -1 if
 * all FS_MAX_FILES slots are already occupied ("disk full"). */
static int save_current_document(void) {
    int slot = bound_slot;
    if (slot < 0) {
        slot = fs_find_empty_slot();
        if (slot < 0) return -1;
    }
    if (!fs_save_slot(slot, text_buf, text_len)) return -1;
    desktop_file_exists[slot] = 1;
    desktop_file_len[slot] = text_len;
    bound_slot = slot;
    return slot;
}

/* Shared Yes/No handling for the confirm dialog, used by both mouse
 * clicks and the Y/N keyboard shortcuts. What "Yes"/"No" actually do
 * depends on why the dialog was opened (confirm_mode). */
static void confirm_yes_action(void) {
    int saved_slot = save_current_document();
    if (confirm_mode == CONFIRM_NEW) {
        text_len = 0; text_buf[0] = 0; bound_slot = -1; ko_ime_reset();
        status = saved_slot >= 0 ? format_saved_status(saved_slot) : t(STR_STORAGE_FULL_NOT_SAVED);
    } else if (confirm_mode == CONFIRM_CLOSE) {
        notepad.open = 0;
        notepad.minimized = 0;
        /* Closing ends this editing session -- clear the buffer so the
         * next time the app icon is double-clicked, it starts blank
         * rather than showing this document's leftover text again. */
        text_len = 0; text_buf[0] = 0; bound_slot = -1; ko_ime_reset();
        status = saved_slot >= 0 ? t(STR_SAVED_AND_CLOSED) : t(STR_STORAGE_FULL_CLOSED_NOT_SAVED);
    }
    confirm_mode = CONFIRM_NONE;
}

static void confirm_no_action(void) {
    if (confirm_mode == CONFIRM_NEW) {
        text_len = 0; text_buf[0] = 0; bound_slot = -1; ko_ime_reset();
        status = t(STR_NEW_DOCUMENT_NOT_SAVED);
    } else if (confirm_mode == CONFIRM_CLOSE) {
        notepad.open = 0;
        notepad.minimized = 0;
        /* Same as above: discard the in-memory buffer on close so a
         * fresh app-icon open doesn't resurrect this session's text. */
        text_len = 0; text_buf[0] = 0; bound_slot = -1; ko_ime_reset();
        status = t(STR_CLOSED_NOT_SAVED);
    }
    confirm_mode = CONFIRM_NONE;
}

/* Window-relative button positions, computed from current geometry. */
static inline int btn_close_x(void) { return notepad.x + notepad.w - 2 - BTN_W; }
static inline int btn_max_x(void)   { return btn_close_x() - BTN_W - BTN_GAP; }
static inline int btn_min_x(void)   { return btn_max_x() - BTN_W - BTN_GAP; }
static inline int btn_y(void)       { return notepad.y + 1; }

static inline int edit_x(void) { return notepad.x + 4; }
static inline int edit_y(void) { return notepad.y + TITLEBAR_H + 12; }
static inline int edit_w(void) { return notepad.w - 8; }
static inline int edit_h(void) { return notepad.h - TITLEBAR_H - 16; }

/* ============================================================
 * Taskbar drawing + hit-testing
 * ============================================================ */
static void draw_taskbar(void) {
    bb_fillrect(0, TASKBAR_Y, VGA_WIDTH, TASKBAR_H, COL_LGRAY);
    for (int i = 0; i < VGA_WIDTH; i++) bb_putpixel(i, TASKBAR_Y, COL_WHITE);

    /* Start button. The classic "raised" 3D bevel is just a light stripe
     * on the top/left edges and a dark stripe on the bottom/right --
     * and flipping which stripe goes where makes it look "pressed in"
     * while the menu is open. This exact four-line trick single-handedly
     * carried the entire aesthetic of 16-bit UI toolkits. We salute it. */
    bb_fillrect(STARTBTN_X, STARTBTN_Y, STARTBTN_W, STARTBTN_H, COL_LGRAY);
    u8 hi = start_menu_open ? COL_DGRAY : COL_WHITE;
    u8 lo = start_menu_open ? COL_WHITE : COL_DGRAY;
    for (int i = 0; i < STARTBTN_W - 1; i++) bb_putpixel(STARTBTN_X + i, STARTBTN_Y, hi);
    for (int j = 0; j < STARTBTN_H - 1; j++) bb_putpixel(STARTBTN_X, STARTBTN_Y + j, hi);
    for (int i = 0; i < STARTBTN_W; i++) bb_putpixel(STARTBTN_X + i, STARTBTN_Y + STARTBTN_H - 1, lo);
    for (int j = 0; j < STARTBTN_H; j++) bb_putpixel(STARTBTN_X + STARTBTN_W - 1, STARTBTN_Y + j, lo);
    /* nudge the label a pixel down-right while "pressed", like it's
     * physically sinking into the taskbar under the weight of your click */
    int press = start_menu_open ? 1 : 0;
    font_draw_string(STARTBTN_X + 3 + press, STARTBTN_Y + 1 + press, "AM", COL_BLACK);

    if (notepad.open && notepad.minimized) {
        /* taskbar button showing the minimized app, with its own mini
         * restore/close glyphs drawn right inside the taskbar button. */
        bb_fillrect(TASKBTN_X, TASKBTN_Y, TASKBTN_W, TASKBTN_H, COL_WHITE);
        bb_rect(TASKBTN_X, TASKBTN_Y, TASKBTN_W, TASKBTN_H, COL_DGRAY);
        font_draw_string(TASKBTN_X + 2, TASKBTN_Y + 1, "N...", COL_BLACK);

        /* restore glyph: two overlapping squares */
        int rx = TASKBTN_X + TASKBTN_W - 20, ry = TASKBTN_Y + 1;
        bb_rect(rx + 2, ry, 6, 6, COL_BLACK);
        bb_rect(rx, ry + 2, 6, 6, COL_BLACK);
        bb_fillrect(rx + 1, ry + 3, 4, 4, COL_WHITE);

        /* close glyph: X */
        int cx = TASKBTN_X + TASKBTN_W - 9, cy = TASKBTN_Y + 1;
        for (int i = 0; i < 7; i++) {
            bb_putpixel(cx + i, cy + i, COL_BLACK);
            bb_putpixel(cx + i, cy + 6 - i, COL_BLACK);
        }
    }

    if (setting.open && setting.minimized) {
        /* same pill, same glyphs, its own slot -- SETTING.EXE's taskbar
         * presence when minimized. */
        bb_fillrect(TASKBTN2_X, TASKBTN2_Y, TASKBTN2_W, TASKBTN2_H, COL_WHITE);
        bb_rect(TASKBTN2_X, TASKBTN2_Y, TASKBTN2_W, TASKBTN2_H, COL_DGRAY);
        font_draw_string(TASKBTN2_X + 2, TASKBTN2_Y + 1, "S...", COL_BLACK);

        int rx = TASKBTN2_X + TASKBTN2_W - 20, ry = TASKBTN2_Y + 1;
        bb_rect(rx + 2, ry, 6, 6, COL_BLACK);
        bb_rect(rx, ry + 2, 6, 6, COL_BLACK);
        bb_fillrect(rx + 1, ry + 3, 4, 4, COL_WHITE);

        int cx = TASKBTN2_X + TASKBTN2_W - 9, cy = TASKBTN2_Y + 1;
        for (int i = 0; i < 7; i++) {
            bb_putpixel(cx + i, cy + i, COL_BLACK);
            bb_putpixel(cx + i, cy + 6 - i, COL_BLACK);
        }
    }
}

static int start_button_hit(int px, int py) {
    return in_rect(px, py, STARTBTN_X, STARTBTN_Y, STARTBTN_W, STARTBTN_H);
}

static int taskbar_restore_hit(int px, int py) {
    if (!(notepad.open && notepad.minimized)) return 0;
    int rx = TASKBTN_X + TASKBTN_W - 20, ry = TASKBTN_Y + 1;
    return in_rect(px, py, rx, ry, 8, 8);
}
static int taskbar_close_hit(int px, int py) {
    if (!(notepad.open && notepad.minimized)) return 0;
    int cx = TASKBTN_X + TASKBTN_W - 9, cy = TASKBTN_Y + 1;
    return in_rect(px, py, cx, cy, 8, 8);
}
static int taskbar_restore_hit2(int px, int py) {
    if (!(setting.open && setting.minimized)) return 0;
    int rx = TASKBTN2_X + TASKBTN2_W - 20, ry = TASKBTN2_Y + 1;
    return in_rect(px, py, rx, ry, 8, 8);
}
static int taskbar_close_hit2(int px, int py) {
    if (!(setting.open && setting.minimized)) return 0;
    int cx = TASKBTN2_X + TASKBTN2_W - 9, cy = TASKBTN2_Y + 1;
    return in_rect(px, py, cx, cy, 8, 8);
}

/* ============================================================
 * Start Menu -- pops up above the AM button, Windows-95-style: a dark
 * vertical banner strip down the left side, a short list of items on
 * the right, and a power icon pinned to the bottom behind a divider,
 * which cascades into a small Shut Down / Restart flyout instead of
 * doing anything on its own -- because even a two-app OS deserves a
 * proper "are you sure" ceremony before it turns itself off.
 * ============================================================ */
#define STARTMENU_BANNER_W  14
#define STARTMENU_W         120
#define STARTMENU_H         74
#define STARTMENU_ITEM_H    12
#define STARTMENU_ITEMS     3   /* 0=Notepad.exe, 1=Setting.exe, 2=power */
#define STARTMENU_POWER_IDX (STARTMENU_ITEMS - 1)

static inline int start_menu_x(void) { return STARTBTN_X; }
static inline int start_menu_y(void) { return TASKBAR_Y - STARTMENU_H; }

/* Notepad/Setting are grouped near the top; the power item sits glued
 * to the bottom edge behind its own divider, exactly like the real
 * thing -- no matter how few programs you have installed, it never
 * moves. */
static inline int start_menu_item_y(int idx) {
    if (idx == STARTMENU_ITEMS - 1)
        return start_menu_y() + STARTMENU_H - STARTMENU_ITEM_H - 3;
    return start_menu_y() + 4 + idx * STARTMENU_ITEM_H;
}

static int start_menu_item_hit(int px, int py, int idx) {
    int x = start_menu_x() + STARTMENU_BANNER_W + 2;
    int w = STARTMENU_W - STARTMENU_BANNER_W - 4;
    return in_rect(px, py, x, start_menu_item_y(idx), w, STARTMENU_ITEM_H);
}

/* An 8x8 hand-drawn "power" glyph -- a ring with a vertical stroke
 * poking through the gap at the top, same silhouette as the standard
 * IEC power symbol. Drawn the same way font glyphs are (row-by-row
 * bitmask), it just isn't in the text font, so it gets its own
 * function instead of a character code. */
static const u8 power_icon_bits[8] = {
    0x18, 0x18, 0x42, 0x81, 0x81, 0x42, 0x3C, 0x00
};
static void draw_power_icon(int x, int y, u8 color) {
    for (int row = 0; row < 8; row++) {
        u8 bits = power_icon_bits[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) bb_putpixel(x + col, y + row, color);
        }
    }
}

/* Whether the Shut Down / Restart flyout is showing. Only ever true
 * while start_menu_open is also true -- it's a cascade OFF the Start
 * Menu, not a thing that can exist on its own. */
static int power_menu_open = 0;

#define POWERMENU_W       80
#define POWERMENU_ITEM_H  12
#define POWERMENU_ITEMS   2   /* 0=Shut Down, 1=Restart */
#define POWERMENU_H       (POWERMENU_ITEMS * POWERMENU_ITEM_H + 6)

/* Cascades off the right edge of the Start Menu, biased toward the
 * bottom (flush with the taskbar) rather than centered -- it's growing
 * out of the power item, which is itself pinned to the Start Menu's
 * bottom edge. */
static inline int power_menu_x(void) { return start_menu_x() + STARTMENU_W; }
static inline int power_menu_y(void) { return TASKBAR_Y - POWERMENU_H; }

static inline int power_menu_item_y(int idx) { return power_menu_y() + 3 + idx * POWERMENU_ITEM_H; }

static int power_menu_item_hit(int px, int py, int idx) {
    return in_rect(px, py, power_menu_x() + 2, power_menu_item_y(idx), POWERMENU_W - 4, POWERMENU_ITEM_H);
}

static void draw_power_menu(int mx, int my) {
    int x = power_menu_x(), y = power_menu_y(), w = POWERMENU_W, h = POWERMENU_H;
    bb_fillrect(x + 2, y + 2, w, h, COL_DGRAY);
    bb_fillrect(x, y, w, h, COL_LGRAY);
    bb_rect(x, y, w, h, COL_BLACK);

    const char *labels[POWERMENU_ITEMS] = { t(STR_SHUT_DOWN), t(STR_RESTART) };
    for (int i = 0; i < POWERMENU_ITEMS; i++) {
        int iy = power_menu_item_y(i);
        int hover = in_rect(mx, my, x + 2, iy, w - 4, POWERMENU_ITEM_H);
        u8 bg = hover ? COL_BLUE : COL_LGRAY;
        u8 fg = hover ? COL_WHITE : COL_BLACK;
        bb_fillrect(x + 2, iy, w - 4, POWERMENU_ITEM_H, bg);
        ko_draw_mixed_string(x + 5, iy + 2, labels[i], fg);
    }
}

static void draw_start_menu(int mx, int my) {
    int x = start_menu_x(), y = start_menu_y();
    int w = STARTMENU_W, h = STARTMENU_H;

    /* same drop-shadow-plus-outline recipe as the File dropdown and the
     * confirm dialog -- three popups, one visual language, zero effort
     * spent reinventing it each time */
    bb_fillrect(x + 2, y + 2, w, h, COL_DGRAY);
    bb_fillrect(x, y, w, h, COL_LGRAY);
    bb_rect(x, y, w, h, COL_BLACK);

    /* left banner: solid navy strip with the product name climbing up
     * it bottom-to-top. This is the single most 1995 thing in this
     * entire codebase and we regret nothing. */
    bb_fillrect(x + 1, y + 1, STARTMENU_BANNER_W - 1, h - 2, COL_BLUE);
    font_draw_string_vertical(x + 3, y + h - 9, "MINIWIN", COL_WHITE);

    const char *labels[2] = {"Notepad.exe", "Setting.exe"};
    int ix = x + STARTMENU_BANNER_W + 2;
    int iw = w - STARTMENU_BANNER_W - 4;
    for (int i = 0; i < STARTMENU_ITEMS; i++) {
        int iy = start_menu_item_y(i);
        /* while the flyout is open, the power row stays highlighted --
         * it's the thing the cascade is coming from, same as a real
         * cascading menu keeps its parent item lit */
        int hover = (i == STARTMENU_POWER_IDX && power_menu_open)
                        || in_rect(mx, my, ix, iy, iw, STARTMENU_ITEM_H);
        u8 bg = hover ? COL_BLUE : COL_LGRAY;
        u8 fg = hover ? COL_WHITE : COL_BLACK;
        bb_fillrect(ix, iy, iw, STARTMENU_ITEM_H, bg);
        if (i == STARTMENU_POWER_IDX) {
            draw_power_icon(ix + 3, iy + 2, fg);
        } else {
            ko_draw_mixed_string(ix + 3, iy + 2, labels[i], fg);
        }
    }

    /* divider directly above the power item, the traditional "heads up,
     * something drastic is about to happen" line */
    int sep_y = start_menu_item_y(STARTMENU_POWER_IDX) - 2;
    for (int i = 0; i < iw; i++) bb_putpixel(ix + i, sep_y, COL_DGRAY);

    if (power_menu_open) draw_power_menu(mx, my);
}

/* ============================================================
 * Notepad window drawing
 * ============================================================ */
static void draw_titlebar_buttons(void) {
    int y = btn_y();

    /* minimize "_" */
    int mnx = btn_min_x();
    bb_fillrect(mnx, y, BTN_W, BTN_H, COL_LGRAY);
    bb_rect(mnx, y, BTN_W, BTN_H, COL_BLACK);
    for (int i = 2; i < BTN_W - 2; i++) bb_putpixel(mnx + i, y + BTN_H - 3, COL_BLACK);

    /* maximize "[]" -- toggles fullscreen (above the taskbar) */
    int mxx = btn_max_x();
    bb_fillrect(mxx, y, BTN_W, BTN_H, COL_LGRAY);
    bb_rect(mxx, y, BTN_W, BTN_H, COL_BLACK);
    bb_rect(mxx + 2, y + 2, BTN_W - 4, BTN_H - 4, COL_BLACK);

    /* close "X" */
    int clx = btn_close_x();
    bb_fillrect(clx, y, BTN_W, BTN_H, COL_LGRAY);
    bb_rect(clx, y, BTN_W, BTN_H, COL_BLACK);
    for (int i = 2; i < BTN_W - 2; i++) {
        bb_putpixel(clx + i, y + 2 + (i - 2), COL_BLACK);
        bb_putpixel(clx + (BTN_W - 1 - i), y + 2 + (i - 2), COL_BLACK);
    }
}

static void draw_window(void) {
    int wx = notepad.x, wy = notepad.y, ww = notepad.w, wh = notepad.h;

    /* drop shadow (skip when maximized -- looks wrong flush with the
     * screen edge and the taskbar) */
    if (!notepad.maximized) {
        bb_fillrect(wx + 3, wy + 3, ww, wh, COL_DGRAY);
    }

    /* window body */
    bb_fillrect(wx, wy, ww, wh, COL_LGRAY);
    bb_rect(wx, wy, ww, wh, COL_BLACK);

    /* title bar */
    bb_fillrect(wx + 1, wy + 1, ww - 2, TITLEBAR_H, COL_BLUE);
    font_draw_string(wx + 3, wy + 1, "NOTEPAD.EXE", COL_WHITE);

    draw_titlebar_buttons();

    /* menu bar */
    int menu_y = wy + TITLEBAR_H + 1;
    bb_fillrect(wx + 1, menu_y, ww - 2, 9, COL_LGRAY);
    if (file_menu_open) {
        /* highlight the File label while its dropdown is open, like a
         * pressed menu button */
        bb_fillrect(wx + 3, menu_y, MENU_FILE_LABEL_W - 2, 9, COL_BLUE);
        ko_draw_mixed_string(wx + 4,  menu_y, t(STR_FILE), COL_WHITE);
    } else {
        ko_draw_mixed_string(wx + 4,  menu_y, t(STR_FILE), COL_BLACK);
    }
    ko_draw_mixed_string(wx + 40, menu_y, t(STR_EDIT), COL_BLACK);
    ko_draw_mixed_string(wx + 76, menu_y, t(STR_HELP), COL_BLACK);
    for (int i = 0; i < ww - 2; i++)
        bb_putpixel(wx + 1 + i, menu_y + 9, COL_DGRAY);

    /* text edit area (white, sunken border) */
    int ex = edit_x(), ey = edit_y(), ew = edit_w(), eh = edit_h();
    bb_fillrect(ex, ey, ew, eh, COL_WHITE);
    bb_rect(ex - 1, ey - 1, ew + 2, eh + 2, COL_DGRAY);

    /* text content -- UTF-8 aware: most bytes are 1 ASCII char, but a
     * 3-byte sequence (0xE0 lead byte) is one Hangul syllable/jamo drawn
     * via the Korean font instead of the Latin one. Both render at the
     * same 8px advance since the Hangul glyphs were extracted at 8x8 to
     * match. */
    int cx = ex + 2, cy = ey + 2;
    for (u32 i = 0; i < text_len; ) {
        int clen = ko_utf8_char_len((unsigned char)text_buf[i]);
        if (clen == 3 && i + 3 <= text_len) {
            if (cx > ex + ew - 10) { cx = ex + 2; cy += 9; }
            if (cy > ey + eh - 8) break;
            int cp = ko_utf8_decode3(&text_buf[i]);
            ko_font_draw_codepoint(cx, cy, cp, COL_BLACK);
            cx += 8;
            i += 3;
            continue;
        }
        char c = text_buf[i];
        if (c == '\n' || cx > ex + ew - 10) {
            cx = ex + 2;
            cy += 9;
            if (c == '\n') { i++; continue; }
        }
        if (cy > ey + eh - 8) break;
        font_draw_char(cx, cy, c, COL_BLACK);
        cx += 8;
        i++;
    }

    /* Live preview of the syllable currently being composed (if Hangul
     * mode is on and something's mid-composition), shown right at the
     * cursor position before it's actually committed to text_buf. */
    if (current_ime == IME_KOREAN && ko_ime_is_composing()) {
        if (cx > ex + ew - 10) { cx = ex + 2; cy += 9; }
        int preview_cp = ko_ime_preview_codepoint();
        if (preview_cp >= 0 && cy <= ey + eh - 8) {
            ko_font_draw_codepoint(cx, cy, preview_cp, COL_BLACK);
            cx += 8; /* advance past the preview glyph so the cursor bar
                      * below is drawn after it, not overlapping it */
        }
    }

    bb_fillrect(cx, cy, 2, 8, COL_BLACK); /* text cursor */
}

/* Is (px,py) over the draggable part of the title bar -- i.e. the title
 * bar itself, but not over any of the three control buttons? */
static int titlebar_drag_hit(int px, int py) {
    if (!in_rect(px, py, notepad.x + 1, notepad.y + 1, notepad.w - 2, TITLEBAR_H)) return 0;
    if (in_rect(px, py, btn_min_x(), btn_y(), BTN_W, BTN_H)) return 0;
    if (in_rect(px, py, btn_max_x(), btn_y(), BTN_W, BTN_H)) return 0;
    if (in_rect(px, py, btn_close_x(), btn_y(), BTN_W, BTN_H)) return 0;
    return 1;
}

/* Generic maximize/restore -- works on any window_t, so both Notepad and
 * SETTING.EXE's window can share one implementation instead of two
 * copies of the same four assignments. */
static void maximize_window(window_t *w) {
    if (w->maximized) return;
    w->restore_x = w->x;
    w->restore_y = w->y;
    w->restore_w = w->w;
    w->restore_h = w->h;
    w->x = MAXIMIZED_X;
    w->y = MAXIMIZED_Y;
    w->w = MAXIMIZED_W;
    w->h = MAXIMIZED_H;
    w->maximized = 1;
}

static void unmaximize_window(window_t *w) {
    if (!w->maximized) return;
    w->x = w->restore_x;
    w->y = w->restore_y;
    w->w = w->restore_w;
    w->h = w->restore_h;
    w->maximized = 0;
}

/* ============================================================
 * SETTING.EXE window -- SYSTEM > Language / IME.
 *
 * Reuses window_t, and now (per popular demand) supports minimize and
 * maximize exactly like Notepad's window does -- same three-button
 * title bar, same taskbar-pill-when-minimized treatment, just its own
 * independent state and its own taskbar slot. (setting's window_t
 * itself, and its default-geometry constants, live up near notepad's
 * declaration -- see the comment there for why.)
 * ============================================================ */
#define SETTING_SIDEBAR_W   74
#define SETTING_NAV_ITEM_H  11
#define SETTING_ROW_W       112
#define SETTING_ROW_H       11
#define SETTING_ROW_GAP     14

#define SETTING_NAV_LANGUAGE 0
#define SETTING_NAV_IME      1

/* Which sidebar page is showing. Persists across close/reopen within
 * the same boot, same as any real settings app remembering your last
 * tab -- nobody wants to re-navigate to Language every single time. */
static int setting_page = SETTING_NAV_LANGUAGE;

static inline int setting_btn_close_x(void) { return setting.x + setting.w - 2 - BTN_W; }
static inline int setting_btn_max_x(void)   { return setting_btn_close_x() - BTN_W - BTN_GAP; }
static inline int setting_btn_min_x(void)   { return setting_btn_max_x() - BTN_W - BTN_GAP; }
static inline int setting_btn_y(void) { return setting.y + 1; }

static int setting_close_hit(int px, int py) {
    return in_rect(px, py, setting_btn_close_x(), setting_btn_y(), BTN_W, BTN_H);
}
static int setting_min_hit(int px, int py) {
    return in_rect(px, py, setting_btn_min_x(), setting_btn_y(), BTN_W, BTN_H);
}
static int setting_max_hit(int px, int py) {
    return in_rect(px, py, setting_btn_max_x(), setting_btn_y(), BTN_W, BTN_H);
}

/* Draggable part of the title bar -- the whole bar except its three
 * buttons, same idea as Notepad's titlebar_drag_hit(). */
static int setting_titlebar_drag_hit(int px, int py) {
    if (!in_rect(px, py, setting.x + 1, setting.y + 1, setting.w - 2, TITLEBAR_H)) return 0;
    if (in_rect(px, py, setting_btn_min_x(), setting_btn_y(), BTN_W, BTN_H)) return 0;
    if (in_rect(px, py, setting_btn_max_x(), setting_btn_y(), BTN_W, BTN_H)) return 0;
    if (in_rect(px, py, setting_btn_close_x(), setting_btn_y(), BTN_W, BTN_H)) return 0;
    return 1;
}

static inline int setting_header_y(void) { return setting.y + TITLEBAR_H + 3; }
static inline int setting_nav_y(int idx) { return setting_header_y() + 10 + idx * SETTING_NAV_ITEM_H; }

static int setting_nav_hit(int px, int py, int idx) {
    return in_rect(px, py, setting.x + 2, setting_nav_y(idx), SETTING_SIDEBAR_W - 3, SETTING_NAV_ITEM_H);
}

static inline int setting_content_x(void) { return setting.x + SETTING_SIDEBAR_W + 4; }
static inline int setting_row_y(int idx) { return setting_header_y() + idx * SETTING_ROW_GAP; }

static int setting_row_hit(int px, int py, int idx) {
    return in_rect(px, py, setting_content_x(), setting_row_y(idx), SETTING_ROW_W, SETTING_ROW_H);
}

/* Builds "(*) Name" / "( ) Name" (radio, single-select -- Language) or
 * "<x> Name" / "< > Name" (checkbox, multi-select -- IME) into `out`.
 * `name` is deliberately each language's OWN name for itself
 * ("English", "한국어") rather than anything from the ui_strings
 * table -- exactly how every real language picker does it, so you can
 * still find your language even if you can't read whichever one is
 * currently active. */
static void build_option_label(char *out, u32 outsz, int is_radio, int selected, const char *name) {
    u32 len = 0;
    /* Angle brackets, not square ones -- the bitmap font only covers
     * ASCII 0x20-0x5A (space through 'Z'), and '[' / ']' fall just
     * outside that range, so they'd silently draw as blank gaps. "<x>"
     * also has the nice side effect of looking visually distinct from
     * the radio buttons' "(*)", which is the whole point of using
     * different bracket styles for single- vs multi-select. */
    const char *pre = is_radio ? (selected ? "(*) " : "( ) ")
                                : (selected ? "<x> " : "< > ");
    while (*pre) kstrcpy_append(out, &len, outsz, *pre++);
    while (*name) kstrcpy_append(out, &len, outsz, *name++);
}

static void draw_setting_window(void) {
    int wx = setting.x, wy = setting.y, ww = setting.w, wh = setting.h;

    if (!setting.maximized) {
        bb_fillrect(wx + 3, wy + 3, ww, wh, COL_DGRAY); /* drop shadow */
    }
    bb_fillrect(wx, wy, ww, wh, COL_LGRAY);
    bb_rect(wx, wy, ww, wh, COL_BLACK);

    bb_fillrect(wx + 1, wy + 1, ww - 2, TITLEBAR_H, COL_BLUE);
    font_draw_string(wx + 3, wy + 1, "SETTING.EXE", COL_WHITE);

    /* three title bar buttons, same glyphs and layout as Notepad's
     * draw_titlebar_buttons() -- kept as its own copy since it draws
     * against setting_btn_*() instead of Notepad's btn_*() */
    int by = setting_btn_y();
    int mnx = setting_btn_min_x();
    bb_fillrect(mnx, by, BTN_W, BTN_H, COL_LGRAY);
    bb_rect(mnx, by, BTN_W, BTN_H, COL_BLACK);
    for (int i = 2; i < BTN_W - 2; i++) bb_putpixel(mnx + i, by + BTN_H - 3, COL_BLACK);

    int mxx = setting_btn_max_x();
    bb_fillrect(mxx, by, BTN_W, BTN_H, COL_LGRAY);
    bb_rect(mxx, by, BTN_W, BTN_H, COL_BLACK);
    bb_rect(mxx + 2, by + 2, BTN_W - 4, BTN_H - 4, COL_BLACK);

    int clx = setting_btn_close_x();
    bb_fillrect(clx, by, BTN_W, BTN_H, COL_LGRAY);
    bb_rect(clx, by, BTN_W, BTN_H, COL_BLACK);
    for (int i = 2; i < BTN_W - 2; i++) {
        bb_putpixel(clx + i, by + 2 + (i - 2), COL_BLACK);
        bb_putpixel(clx + (BTN_W - 1 - i), by + 2 + (i - 2), COL_BLACK);
    }

    /* sidebar / content divider */
    int body_y = wy + TITLEBAR_H + 1;
    int body_h = wh - TITLEBAR_H - 2;
    for (int j = 0; j < body_h; j++) bb_putpixel(wx + SETTING_SIDEBAR_W, body_y + j, COL_DGRAY);

    /* "SYSTEM" section header, then the two navigable pages under it */
    ko_draw_mixed_string(wx + 3, body_y + 2, t(STR_SYSTEM), COL_BLACK);
    const char *nav_labels[2] = { t(STR_LANGUAGE), t(STR_IME) };
    for (int i = 0; i < 2; i++) {
        int ny = setting_nav_y(i);
        int active = (setting_page == i);
        u8 bg = active ? COL_BLUE : COL_LGRAY;
        u8 fg = active ? COL_WHITE : COL_BLACK;
        bb_fillrect(wx + 2, ny, SETTING_SIDEBAR_W - 3, SETTING_NAV_ITEM_H, bg);
        ko_draw_mixed_string(wx + 6, ny + 1, nav_labels[i], fg);
    }

    /* content area for whichever page is active */
    char label[24];
    if (setting_page == SETTING_NAV_LANGUAGE) {
        build_option_label(label, sizeof(label), 1, sys_language == LANG_ENGLISH, "English");
        ko_draw_mixed_string(setting_content_x(), setting_row_y(0), label, COL_BLACK);
        build_option_label(label, sizeof(label), 1, sys_language == LANG_KOREAN, "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4");
        ko_draw_mixed_string(setting_content_x(), setting_row_y(1), label, COL_BLACK);
    } else {
        build_option_label(label, sizeof(label), 0, ime_enabled[IME_ENGLISH], "English");
        ko_draw_mixed_string(setting_content_x(), setting_row_y(0), label, COL_BLACK);
        build_option_label(label, sizeof(label), 0, ime_enabled[IME_KOREAN], "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4");
        ko_draw_mixed_string(setting_content_x(), setting_row_y(1), label, COL_BLACK);
    }
}

/* ============================================================
 * File menu dropdown + Save/New confirm dialog drawing
 * ============================================================ */
static void draw_file_menu(int mx, int my) {
    int x = file_menu_x();
    int y = file_menu_top_y();
    int w = FILE_MENU_W;
    int h = FILE_MENU_ITEM_H * 3;

    bb_fillrect(x + 2, y + 2, w, h, COL_DGRAY); /* drop shadow */
    bb_fillrect(x, y, w, h, COL_LGRAY);
    bb_rect(x, y, w, h, COL_BLACK);

    const char *labels[3] = { t(STR_SAVE_AS), t(STR_SAVE), t(STR_NEW) };
    for (int i = 0; i < 3; i++) {
        int iy = y + i * FILE_MENU_ITEM_H;
        int hover = in_rect(mx, my, x, iy, w, FILE_MENU_ITEM_H);
        u8 bg = hover ? COL_BLUE : COL_LGRAY;
        u8 fg = hover ? COL_WHITE : COL_BLACK;
        bb_fillrect(x + 1, iy, w - 2, FILE_MENU_ITEM_H, bg);
        ko_draw_mixed_string(x + 4, iy + 1, labels[i], fg);
    }
}

/* A small yellow warning triangle with a black "!" inside -- built out
 * of scanlines and a couple of fillrects, since one hand-drawn icon
 * doesn't justify writing a general polygon rasterizer. Sits to the
 * left of the confirm dialog's message so "Save changes?" actually
 * looks like it's asking something, not just stating it. */
static void draw_warning_icon(int x, int y) {
    int h = 13;
    for (int row = 0; row < h; row++) {
        int half = (row * 6) / (h - 1); /* 0..6: widens going down */
        bb_fillrect(x + 6 - half, y + row, half * 2 + 1, 1, COL_YELLOW);
        bb_putpixel(x + 6 - half, y + row, COL_BLACK);
        bb_putpixel(x + 6 + half, y + row, COL_BLACK);
    }
    for (int i = 0; i <= 12; i++) bb_putpixel(x + i, y + h - 1, COL_BLACK);
    bb_fillrect(x + 5, y + 3, 2, 5, COL_BLACK); /* the "!" stem */
    bb_fillrect(x + 5, y + 9, 2, 2, COL_BLACK); /* the "!" dot */
}

static void draw_confirm_dialog(int mx, int my) {
    int x = confirm_x(), y = confirm_y();

    bb_fillrect(x + 3, y + 3, CONFIRM_W, CONFIRM_H, COL_DGRAY);
    bb_fillrect(x, y, CONFIRM_W, CONFIRM_H, COL_LGRAY);
    bb_rect(x, y, CONFIRM_W, CONFIRM_H, COL_BLACK);

    /* Title bar -- deliberately just an X, no minimize/maximize. This is
     * a modal warning, not a document window; the only two things you
     * can do with it are answer it or dismiss it. */
    bb_fillrect(x + 1, y + 1, CONFIRM_W - 2, TITLEBAR_H, COL_BLUE);
    font_draw_string(x + 3, y + 1, "Warning!", COL_WHITE);
    int clx = confirm_close_x(), cy = confirm_close_y();
    bb_fillrect(clx, cy, BTN_W, BTN_H, COL_LGRAY);
    bb_rect(clx, cy, BTN_W, BTN_H, COL_BLACK);
    for (int i = 2; i < BTN_W - 2; i++) {
        bb_putpixel(clx + i, cy + i - 1, COL_BLACK);
        bb_putpixel(clx + (BTN_W - 1 - i), cy + i - 1, COL_BLACK);
    }

    int body_y = y + TITLEBAR_H + 3;
    draw_warning_icon(x + 8, body_y);
    ko_draw_mixed_string(x + 26, body_y + 1,  t(STR_SAVE_CHANGES), COL_BLACK);
    if (confirm_mode == CONFIRM_CLOSE) {
        ko_draw_mixed_string(x + 26, body_y + 11, t(STR_BEFORE_CLOSING), COL_BLACK);
    } else {
        ko_draw_mixed_string(x + 26, body_y + 11, t(STR_BEFORE_NEW), COL_BLACK);
    }

    int yb = confirm_btn_y();
    int yesx = confirm_yes_x(), nox = confirm_no_x();
    int yes_hover = in_rect(mx, my, yesx, yb, CONFIRM_BTN_W, CONFIRM_BTN_H);
    int no_hover  = in_rect(mx, my, nox,  yb, CONFIRM_BTN_W, CONFIRM_BTN_H);

    /* Yes/No labels are centered in their buttons via ko_string_width()
     * rather than hardcoded offsets, since "아니오" isn't the same pixel
     * width as "No" -- can't just reuse the English magic numbers. */
    const char *yes_label = t(STR_YES), *no_label = t(STR_NO);
    int yes_tx = yesx + (CONFIRM_BTN_W - ko_string_width(yes_label)) / 2;
    int no_tx  = nox  + (CONFIRM_BTN_W - ko_string_width(no_label)) / 2;

    bb_fillrect(yesx, yb, CONFIRM_BTN_W, CONFIRM_BTN_H, yes_hover ? COL_BLUE : COL_WHITE);
    bb_rect(yesx, yb, CONFIRM_BTN_W, CONFIRM_BTN_H, COL_BLACK);
    ko_draw_mixed_string(yes_tx, yb + 2, yes_label, yes_hover ? COL_WHITE : COL_BLACK);

    bb_fillrect(nox, yb, CONFIRM_BTN_W, CONFIRM_BTN_H, no_hover ? COL_BLUE : COL_WHITE);
    bb_rect(nox, yb, CONFIRM_BTN_W, CONFIRM_BTN_H, COL_BLACK);
    ko_draw_mixed_string(no_tx, yb + 2, no_label, no_hover ? COL_WHITE : COL_BLACK);
}

/* ============================================================
 * Desktop file icons -- one per occupied slot (up to FS_MAX_FILES),
 * arranged in a row below the app icon since the 320x200 screen has much
 * more spare width than height. Labels are short ("DOC", "DOC2", ...)
 * rather than the full filename, since there isn't room for 4 full
 * "NEW_TXT_DOC_N.TXT" labels side by side -- the real saved filename is
 * still the proper one on disk and in status messages, this is just a
 * compact on-screen label.
 * ============================================================ */
#define FILEICON_ROW_Y      (ICON_Y + ICON_H + 6)
#define FILEICON_SPACING_X  34

static const char *fileicon_labels[FS_MAX_FILES] = {"DOC", "DOC2", "DOC3", "DOC4"};

static inline int fileicon_x(int slot) { return ICON_X + slot * FILEICON_SPACING_X; }
static inline int fileicon_y(int slot) { (void)slot; return FILEICON_ROW_Y; }

static void draw_desktop_file_icons(void) {
    for (int slot = 0; slot < FS_MAX_FILES; slot++) {
        if (!desktop_file_exists[slot]) continue;
        int x = fileicon_x(slot), y = fileicon_y(slot);
        bb_fillrect(x + 4, y, 16, 12, COL_WHITE);
        bb_rect(x + 4, y, 16, 12, COL_BLACK);
        bb_fillrect(x + 4 + 11, y, 5, 4, DESKTOP_COLOR_ICON_BG);
        bb_rect(x + 4 + 11, y, 5, 4, COL_BLACK);
        /* a couple of horizontal "text lines" inside so it reads as a
         * document icon, visually distinct from the app icon above it */
        bb_fillrect(x + 7, y + 4, 8, 1, COL_LGRAY);
        bb_fillrect(x + 7, y + 7, 8, 1, COL_LGRAY);
        font_draw_string(x, y + 14, fileicon_labels[slot], COL_BLACK);
    }
}

/* ============================================================
 * Mouse cursor
 * ============================================================ */
static const char cursor_shape[11][7] = {
    "X......","XX.....","X.X....","X..X...","X...X..",
    "X....X.","X.....X","X....XX","X..X.X.","X.X..X.","XX...X.",
};

static void draw_cursor(int x, int y) {
    for (int j = 0; j < 11; j++)
        for (int i = 0; i < 7; i++)
            if (cursor_shape[j][i] == 'X')
                bb_putpixel(x + i, y + j, COL_BLACK);
}

/* ============================================================
 * Frame composition
 * ============================================================ */
static void draw_status_line(const char *msg) {
    ko_draw_mixed_string(4, VGA_HEIGHT - TASKBAR_H - 9, msg, COL_BLACK);
}

static void render_frame(int mouse_x, int mouse_y, const char *status_msg) {
    bb_fillrect(0, 0, VGA_WIDTH, VGA_HEIGHT, DESKTOP_COLOR_BG);

    draw_desktop_icon();
    draw_desktop_icon2();
    draw_desktop_file_icons();

    if (status_msg) draw_status_line(status_msg);

    if (notepad.open && !notepad.minimized) {
        draw_window();
        if (file_menu_open) draw_file_menu(mouse_x, mouse_y);
        if (confirm_mode != CONFIRM_NONE) draw_confirm_dialog(mouse_x, mouse_y);
    }

    /* drawn after Notepad, so it sits on top when both happen to be open
     * and overlapping */
    if (setting.open && !setting.minimized) draw_setting_window();

    draw_taskbar();
    /* drawn after the taskbar (and after Notepad's window above) so it
     * sits on top of both -- popups always win the z-order argument */
    if (start_menu_open) draw_start_menu(mouse_x, mouse_y);
    draw_cursor(mouse_x, mouse_y);

    vga_present();
}

/* ============================================================
 * Power management -- as real as a kernel this size can make it. No
 * ACPI, no APM, no drivers: just the two tricks that actually worked
 * on bare x86 for decades before any of that existed.
 * ============================================================ */

/* Real reboot: pulse the keyboard controller's reset line. The 8042
 * chip has a spare output pin wired straight to the CPU's RESET input --
 * BIOSes and boot sectors have used this exact trick since long before
 * ACPI existed, and it still works in QEMU and on real hardware alike. */
static void system_restart(void) {
    while (inb(0x64) & 0x02) { } /* wait for the input buffer to clear */
    outb(0x64, 0xFE);
    for (;;) { __asm__ volatile ("hlt"); } /* belt-and-suspenders, in case the pulse didn't take */
}

/* Real shutdown, 1990s-honest: this kernel has no ACPI power-off, so
 * rather than fake one, it does exactly what pre-ACPI PCs actually did
 * -- paint the classic message and physically stop the CPU. The
 * message was never a lie on hardware like that; it's not one here
 * either. */
static void system_shutdown(void) {
    __asm__ volatile ("cli");
    bb_fillrect(0, 0, VGA_WIDTH, VGA_HEIGHT, COL_BLACK);
    ko_draw_mixed_string(20, 96, t(STR_SAFE_TO_TURN_OFF), COL_WHITE);
    vga_present();
    for (;;) { __asm__ volatile ("hlt"); }
}

/* ============================================================
 * Kernel entry
 * ============================================================ */
void kmain(void) {
    vga_set_mode13h();
    vga_set_standard_palette();
    mouse_init();

    /* Diagnostic-only for now: dumps every PCI device found (including
     * whatever network controller QEMU is presenting) out over the
     * serial port, so real hardware IDs can be confirmed before writing
     * a driver against them. Doesn't touch the GUI at all. */
    serial_init();
    pci_scan();

    /* Check for previously-saved files on disk (real, persistent storage
     * via the ATA driver -- this survives across QEMU runs as long as the
     * disk image itself isn't rebuilt from scratch). */
    for (int slot = 0; slot < FS_MAX_FILES; slot++) {
        u32 loaded_len = 0;
        if (fs_check_slot(slot, &loaded_len)) {
            desktop_file_exists[slot] = 1;
            desktop_file_len[slot] = loaded_len;
        }
    }

    int mx = VGA_WIDTH / 2, my = VGA_HEIGHT / 2;
    int awaiting_second_click = 0;   /* 1 = one icon click seen, waiting for a 2nd within the window */
    u32 last_icon_click_tick = 0;
    int awaiting_second_click_setting = 0;   /* same idea, but for the SETTING.EXE icon */
    u32 last_setting_click_tick = 0;
    int awaiting_second_click_file_slot = -1;   /* which file icon (if any) saw a first click */
    u32 last_file_icon_click_tick = 0;
    u32 tick = 0;
    const u32 double_click_window = 60000; /* tuned for delay(2000) per frame;
                                             * adjust proportionally if delay() changes */

    /* Window-drag state: while dragging, we track the offset from the
     * window's top-left corner to the point the user grabbed, so the
     * window follows the cursor without "snapping" its corner to it. */
    int dragging = 0;
    int drag_offset_x = 0, drag_offset_y = 0;

    /* Same idea, separate state, for SETTING.EXE's window -- it can be
     * dragged independently of (and simultaneously open alongside)
     * Notepad. */
    int dragging_setting = 0;
    int drag_offset_x2 = 0, drag_offset_y2 = 0;

    render_frame(mx, my, status);

    while (1) {
        tick++;

        /* ---- mouse ---- */
        if (mouse_poll()) {
            mx += mouse_dx;
            my += mouse_dy;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx > VGA_WIDTH - 1)  mx = VGA_WIDTH - 1;
            if (my > VGA_HEIGHT - 1) my = VGA_HEIGHT - 1;

            int left_now = mouse_left;
            int clicked = mouse_click_event; /* per-packet edge detection from the driver */

            /* ---- drag in progress: move the window with the cursor ---- */
            if (dragging) {
                if (left_now) {
                    if (!notepad.maximized) {
                        int new_x = mx - drag_offset_x;
                        int new_y = my - drag_offset_y;
                        /* keep at least a sliver of the title bar on-screen
                         * so the window can never be dragged somewhere the
                         * user can't grab it again */
                        notepad.x = clampi(new_x, -(notepad.w - 20), VGA_WIDTH - 20);
                        notepad.y = clampi(new_y, 0, VGA_HEIGHT - TASKBAR_H - TITLEBAR_H);
                    }
                } else {
                    dragging = 0; /* button released -> stop dragging */
                }
            }
            if (dragging_setting) {
                if (left_now) {
                    int new_x = mx - drag_offset_x2;
                    int new_y = my - drag_offset_y2;
                    setting.x = clampi(new_x, -(setting.w - 20), VGA_WIDTH - 20);
                    setting.y = clampi(new_y, 0, VGA_HEIGHT - TASKBAR_H - TITLEBAR_H);
                } else {
                    dragging_setting = 0;
                }
            }

            if (clicked) {
                if (notepad.open && !notepad.minimized && confirm_mode != CONFIRM_NONE) {
                    /* Modal: while a Save-changes confirm dialog is open,
                     * it takes total precedence -- the user must explicitly
                     * choose Yes or No. Clicks elsewhere are ignored. */
                    if (confirm_yes_hit(mx, my)) {
                        confirm_yes_action();
                    } else if (confirm_no_hit(mx, my)) {
                        confirm_no_action();
                    } else if (confirm_close_hit(mx, my)) {
                        /* Cancel: neither save nor discard, just close the
                         * dialog and go back to whatever was on-screen. */
                        confirm_mode = CONFIRM_NONE;
                    }
                } else if (notepad.open && !notepad.minimized && file_menu_open) {
                    /* Modal-ish: while the File dropdown is open, a click
                     * either picks an item or dismisses the menu -- it
                     * doesn't fall through to icon/titlebar/etc handling
                     * this same frame. */
                    if (file_menu_item_hit(mx, my, 0)) {
                        status = t(STR_SAVE_AS_COMING_SOON);
                        file_menu_open = 0;
                    } else if (file_menu_item_hit(mx, my, 1)) {
                        int saved_slot = save_current_document();
                        status = saved_slot >= 0 ? format_saved_status(saved_slot) : t(STR_STORAGE_FULL_NOT_SAVED);
                        file_menu_open = 0;
                    } else if (file_menu_item_hit(mx, my, 2)) {
                        confirm_mode = CONFIRM_NEW;
                        beep_warning();
                        file_menu_open = 0;
                    } else {
                        file_menu_open = 0; /* click outside just dismisses it */
                    }
                } else if (power_menu_open) {
                    /* Cascaded off the Start Menu's power item. Whatever
                     * this click was for -- an action or a miss -- both
                     * menus close afterward, same as clicking a Start
                     * Menu item normally would. */
                    if (power_menu_item_hit(mx, my, 0)) {
                        system_shutdown(); /* does not return */
                    } else if (power_menu_item_hit(mx, my, 1)) {
                        system_restart(); /* does not return */
                    }
                    power_menu_open = 0;
                    start_menu_open = 0;
                } else if (start_menu_open) {
                    /* Same "pick an item or dismiss" contract as the File
                     * dropdown just above -- except the power item, which
                     * cascades into its own flyout instead of resolving
                     * immediately, so it deliberately does NOT close the
                     * Start Menu the way the other two items do. */
                    if (start_menu_item_hit(mx, my, 0)) {
                        notepad.open = 1;
                        notepad.minimized = 0;
                        ko_ime_reset();
                        status = t(STR_NOTEPAD_OPENED);
                        start_menu_open = 0;
                    } else if (start_menu_item_hit(mx, my, 1)) {
                        setting.open = 1;
                        status = t(STR_SETTING_OPENED);
                        start_menu_open = 0;
                    } else if (start_menu_item_hit(mx, my, STARTMENU_POWER_IDX)) {
                        power_menu_open = 1;
                    } else {
                        start_menu_open = 0; /* click outside dismisses everything */
                    }
                } else if (start_button_hit(mx, my)) {
                    start_menu_open = 1;
                } else if (setting.open && !setting.minimized && in_rect(mx, my, setting.x, setting.y, setting.w, setting.h)) {
                    /* SETTING.EXE is a normal (non-modal) window: unlike
                     * the dropdowns/dialogs above, a click that misses
                     * every control inside it just does nothing -- it
                     * does NOT swallow clicks outside its own rect, so
                     * Notepad/icons/taskbar all stay reachable while this
                     * is open. That's what the outer in_rect() above is
                     * for. */
                    if (setting_min_hit(mx, my)) {
                        setting.minimized = 1;
                        status = t(STR_SETTING_MINIMIZED);
                    } else if (setting_max_hit(mx, my)) {
                        if (setting.maximized) {
                            unmaximize_window(&setting);
                            status = t(STR_SETTING_RESTORED);
                        } else {
                            maximize_window(&setting);
                            status = t(STR_SETTING_MAXIMIZED);
                        }
                    } else if (setting_close_hit(mx, my)) {
                        setting.open = 0;
                    } else if (setting_nav_hit(mx, my, SETTING_NAV_LANGUAGE)) {
                        setting_page = SETTING_NAV_LANGUAGE;
                    } else if (setting_nav_hit(mx, my, SETTING_NAV_IME)) {
                        setting_page = SETTING_NAV_IME;
                    } else if (setting_page == SETTING_NAV_LANGUAGE && setting_row_hit(mx, my, 0)) {
                        sys_language = LANG_ENGLISH;
                        status = t(STR_DEFAULT_HINT);
                    } else if (setting_page == SETTING_NAV_LANGUAGE && setting_row_hit(mx, my, 1)) {
                        sys_language = LANG_KOREAN;
                        status = t(STR_DEFAULT_HINT);
                    } else if (setting_page == SETTING_NAV_IME && setting_row_hit(mx, my, 0)) {
                        /* multi-select checkbox -- refuse to uncheck the
                         * last remaining enabled IME, same as real OSes
                         * never let you remove your only keyboard layout */
                        if (ime_enabled[IME_ENGLISH] && !ime_enabled[IME_KOREAN]) {
                            status = t(STR_IME_MIN_ONE);
                        } else {
                            ime_enabled[IME_ENGLISH] = !ime_enabled[IME_ENGLISH];
                            ime_ensure_current_enabled();
                        }
                    } else if (setting_page == SETTING_NAV_IME && setting_row_hit(mx, my, 1)) {
                        if (ime_enabled[IME_KOREAN] && !ime_enabled[IME_ENGLISH]) {
                            status = t(STR_IME_MIN_ONE);
                        } else {
                            ime_enabled[IME_KOREAN] = !ime_enabled[IME_KOREAN];
                            ime_ensure_current_enabled();
                        }
                    } else if (setting_titlebar_drag_hit(mx, my) && !setting.maximized) {
                        dragging_setting = 1;
                        drag_offset_x2 = mx - setting.x;
                        drag_offset_y2 = my - setting.y;
                    }
                } else if (in_rect(mx, my, ICON_X, ICON_Y, ICON_W, ICON_H)) {
                    if (awaiting_second_click && (tick - last_icon_click_tick) < double_click_window) {
                        /* this is the 2nd click of a double-click: open it */
                        notepad.open = 1;
                        notepad.minimized = 0;
                        ko_ime_reset();
                        status = t(STR_NOTEPAD_OPENED);
                        awaiting_second_click = 0;
                    } else {
                        /* this is a 1st click: arm the double-click window */
                        awaiting_second_click = 1;
                        last_icon_click_tick = tick;
                    }
                } else if (in_rect(mx, my, ICON2_X, ICON2_Y, ICON2_W, ICON2_H)) {
                    if (awaiting_second_click_setting && (tick - last_setting_click_tick) < double_click_window) {
                        setting.open = 1;
                        status = t(STR_SETTING_OPENED);
                        awaiting_second_click_setting = 0;
                    } else {
                        awaiting_second_click_setting = 1;
                        last_setting_click_tick = tick;
                    }
                } else if (notepad.open && !notepad.minimized) {
                    if (in_rect(mx, my, btn_min_x(), btn_y(), BTN_W, BTN_H)) {
                        notepad.minimized = 1;
                        status = t(STR_NOTEPAD_MINIMIZED);
                    } else if (in_rect(mx, my, btn_max_x(), btn_y(), BTN_W, BTN_H)) {
                        if (notepad.maximized) {
                            unmaximize_window(&notepad);
                            status = t(STR_NOTEPAD_RESTORED);
                        } else {
                            maximize_window(&notepad);
                            status = t(STR_NOTEPAD_MAXIMIZED);
                        }
                    } else if (in_rect(mx, my, btn_close_x(), btn_y(), BTN_W, BTN_H)) {
                        /* Ask before closing, same Yes/No pattern as New. */
                        confirm_mode = CONFIRM_CLOSE;
                        beep_warning();
                    } else if (file_label_hit(mx, my)) {
                        file_menu_open = 1;
                    } else if (titlebar_drag_hit(mx, my) && !notepad.maximized) {
                        /* start dragging: remember the grab offset so the
                         * window doesn't jump when the drag begins */
                        dragging = 1;
                        drag_offset_x = mx - notepad.x;
                        drag_offset_y = my - notepad.y;
                    }
                } else if (taskbar_restore_hit(mx, my)) {
                    notepad.minimized = 0;
                    status = t(STR_NOTEPAD_RESTORED);
                } else if (taskbar_close_hit(mx, my)) {
                    /* Minimized taskbar close also asks first, for
                     * consistency with the window's own X button. */
                    confirm_mode = CONFIRM_CLOSE;
                    beep_warning();
                    notepad.minimized = 0; /* bring it back on-screen so the dialog is visible */
                } else if (taskbar_restore_hit2(mx, my)) {
                    setting.minimized = 0;
                    status = t(STR_SETTING_RESTORED);
                } else if (taskbar_close_hit2(mx, my)) {
                    /* No unsaved-changes concept in Settings, so its
                     * taskbar close just closes -- no warning needed. */
                    setting.open = 0;
                    setting.minimized = 0;
                } else {
                    /* Check desktop file icons last (any slot). */
                    for (int slot = 0; slot < FS_MAX_FILES; slot++) {
                        if (!desktop_file_exists[slot]) continue;
                        if (!in_rect(mx, my, fileicon_x(slot), fileicon_y(slot), ICON_W, ICON_H)) continue;

                        if (awaiting_second_click_file_slot == slot &&
                            (tick - last_file_icon_click_tick) < double_click_window) {
                            /* 2nd click: open Notepad (if needed) and load
                             * this slot's saved contents into the text
                             * buffer, replacing whatever was there
                             * unsaved, and bind the document to this slot
                             * so a subsequent Save updates it in place. */
                            u32 loaded = fs_load_slot(slot, text_buf, sizeof(text_buf) - 1);
                            text_buf[loaded] = 0;
                            text_len = loaded;
                            bound_slot = slot;
                            notepad.open = 1;
                            notepad.minimized = 0;
                            ko_ime_reset();
                            status = format_saved_status(slot); /* reuse "SAVED: name" wording to show which file opened */
                            awaiting_second_click_file_slot = -1;
                        } else {
                            awaiting_second_click_file_slot = slot;
                            last_file_icon_click_tick = tick;
                        }
                        break;
                    }
                }
            }
        }

        /* ---- keyboard: only affects notepad text when it's open+visible ---- */
        int k = keyboard_poll_key();

        if (confirm_mode != CONFIRM_NONE) {
            /* keyboard shortcuts for the confirm dialog, so it can be
             * driven without the mouse too */
            if (k == 'y' || k == 'Y') {
                confirm_yes_action();
            } else if (k == 'n' || k == 'N') {
                confirm_no_action();
            }
        } else if (k == KEY_RALT && notepad.open && !notepad.minimized && !file_menu_open) {
            /* Right Alt cycles to the next ENABLED input method (see
             * SETTING.EXE > SYSTEM > IME) -- matches the 한/영 key
             * position on real Korean keyboards. F7 used to do this too,
             * but that's gone now that IME selection lives in Settings. */
            ime_cycle_next();
            status = (current_ime == IME_KOREAN) ? t(STR_HANGUL_MODE_ON) : t(STR_ENGLISH_MODE_ON);
        } else if (k > 0 && notepad.open && !notepad.minimized && !file_menu_open) {
            char c = (char)k;
            if (keyboard_ctrl_held()) {
                /* Ctrl+S / Ctrl+N / Ctrl+W accelerators, mirroring the
                 * File menu and the title bar's close button */
                if (ko_ime_is_composing()) ko_ime_commit(text_buf, &text_len, sizeof(text_buf));
                if (c == 's' || c == 'S') {
                    int saved_slot = save_current_document();
                    status = saved_slot >= 0 ? format_saved_status(saved_slot) : t(STR_STORAGE_FULL_NOT_SAVED);
                } else if (c == 'n' || c == 'N') {
                    confirm_mode = CONFIRM_NEW;
                    beep_warning();
                } else if (c == 'w' || c == 'W') {
                    confirm_mode = CONFIRM_CLOSE;
                    beep_warning();
                }
            } else if (c == '\b') {
                if (current_ime == IME_KOREAN && ko_ime_backspace()) {
                    /* consumed by the IME: undid one step of the syllable
                     * currently being composed, nothing else to do */
                } else if (text_len > 0) {
                    int del = ko_utf8_last_char_len(text_buf, text_len);
                    text_len -= del;
                    text_buf[text_len] = 0;
                }
            } else if (current_ime == IME_KOREAN && ko_ime_feed_key(c, text_buf, &text_len, sizeof(text_buf))) {
                /* consumed as a jamo keystroke -- composition state
                 * updated (and/or a completed syllable was appended to
                 * text_buf) inside ko_ime_feed_key() itself */
            } else if (c == '\n' || c == ' ' || (c >= 32 && c < 127)) {
                if (current_ime == IME_KOREAN && ko_ime_is_composing()) {
                    /* a non-jamo key (space, enter, punctuation) always
                     * flushes an in-progress syllable first, matching how
                     * every real Hangul IME behaves */
                    ko_ime_commit(text_buf, &text_len, sizeof(text_buf));
                }
                kstrcpy_append(text_buf, &text_len, sizeof(text_buf), c);
            }
        }

        render_frame(mx, my, status);
        delay(2000); /* lowered further from 8000 -- mouse felt sluggish/
                      * capped at 8000; this loop's real-world speed varies
                      * a lot by CPU, so double_click_window above is
                      * scaled proportionally to keep the same real-time
                      * double-click window. */
    }
}
