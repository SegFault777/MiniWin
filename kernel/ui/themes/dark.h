/* ui/themes/dark.h -- a deliberately different theme: charcoal surfaces, teal accent, a slightly taller title bar.
 * Build with  THEME=dark ./build.sh . Its job is partly diagnostic: any element that still looks "classic"
 * under this theme is a color some module hard-coded instead of asking the theme. */

#define TH_DESKTOP        0x1E2A38u
#define TH_DESKTOP_TEXT   0xE6E9EDu     /* icon captions and the status line, drawn straight on the desktop */
#define TH_FACE           0x3A3F47u
#define TH_LIGHT          0x6B727Du
#define TH_SHADOW         0x15181Cu
#define TH_OUTLINE        0x0A0C0Fu
#define TH_TEXT           0xE6E9EDu
#define TH_TEXT_INVERSE   0xFFFFFFu
#define TH_ACCENT         0x1B7F8Bu
#define TH_ACCENT_TEXT    0xFFFFFFu
#define TH_FIELD          0x23272Eu
#define TH_FIELD_TEXT     0xE6E9EDu
#define TH_CONSOLE        0x0B0E12u
#define TH_CONSOLE_TEXT   0x7CE08Au
#define TH_GLYPH          0xE6E9EDu
#define TH_GLYPH_ON_FIELD 0xE6E9EDu     /* ink for glyphs on a TH_FIELD surface (taskbar pills) */
#define TH_WARNING        0xF2B93Bu
#define TH_WARNING_INK    0x0A0C0Fu
#define TH_SCROLL_TRACK   0x15181Cu
#define TH_SCROLL_THUMB   0x6B727Du
#define TH_CURSOR         0xFFFFFFu

#define TH_TITLEBAR_H     16
#define TH_BTN_W          13
#define TH_BTN_H          12
#define TH_BTN_GAP        2
#define TH_TASKBAR_H      18
#define TH_WINDOW_SHADOW  3
#define TH_MENU_SHADOW    2
