/* ui/themes/classic.h -- the original MiniWin look: Windows-95 gray on a cyan desktop. Pixel-for-pixel what
 * the kernel drew before themes existed (the golden screenshots in tools/test/golden are this theme). */

/* ---- color roles ---- */
#define TH_DESKTOP        COL_LCYAN     /* the desktop background, and the matching icon backing */
#define TH_DESKTOP_TEXT   COL_BLACK     /* icon captions and the status line, drawn straight on the desktop */
#define TH_FACE           COL_LGRAY     /* the surface of windows, menus, buttons, the taskbar */
#define TH_LIGHT          COL_WHITE     /* bevel highlight (top/left of a raised thing) */
#define TH_SHADOW         COL_DGRAY     /* bevel lowlight, drop shadows, dividers, sunken-field border */
#define TH_OUTLINE        COL_BLACK     /* the 1px frame around windows, menus, dialogs */
#define TH_TEXT           COL_BLACK     /* text on a TH_FACE surface */
#define TH_TEXT_INVERSE   COL_WHITE     /* text on a TH_ACCENT surface */
#define TH_ACCENT         COL_BLUE      /* title bars, selected rows, focus rings */
#define TH_ACCENT_TEXT    COL_WHITE     /* the title text itself */
#define TH_FIELD          COL_WHITE     /* a typeable field's background (Notepad, address bar) */
#define TH_FIELD_TEXT     COL_BLACK
#define TH_CONSOLE        COL_BLACK     /* the Terminal's screen */
#define TH_CONSOLE_TEXT   COL_WHITE
#define TH_GLYPH          COL_BLACK     /* the _ [] X drawn on title-bar buttons */
#define TH_GLYPH_ON_FIELD COL_BLACK     /* ink for glyphs on a TH_FIELD surface (taskbar pills) */
#define TH_WARNING        COL_YELLOW    /* the dialog's warning triangle */
#define TH_WARNING_INK    COL_BLACK
#define TH_SCROLL_TRACK   COL_DGRAY
#define TH_SCROLL_THUMB   COL_LGRAY
#define TH_CURSOR         COL_BLACK     /* the mouse pointer's ink */

/* ---- metrics (pixels) ---- */
#define TH_TITLEBAR_H     15
#define TH_BTN_W          13
#define TH_BTN_H          12
#define TH_BTN_GAP        2
#define TH_TASKBAR_H      18
#define TH_WINDOW_SHADOW  3             /* drop-shadow offset of windows and dialogs */
#define TH_MENU_SHADOW    2             /* ... and of menus */
