#ifndef MW_UI_THEME_H
#define MW_UI_THEME_H

/* ui/theme.h -- the OS's look, in one place.
 *
 * WHY THIS EXISTS: before pre-29 every window drew its own frame with its own copy of "COL_LGRAY, COL_BLUE,
 * COL_DGRAY..." and its own copy of the title-bar buttons. Changing the look meant editing six files and
 * hoping you found every COL_ constant. Now:
 *   - themes/<name>.h   defines the COLOR ROLES (TH_FACE, TH_ACCENT, ...) and the METRICS (TH_TITLEBAR_H, ...).
 *                       That file is the whole design; nothing else in the kernel names a raw color.
 *   - ui/widgets.h      draws every piece of chrome (window frame, bevel button, sunken field, menu panel,
 *                       highlighted row, scrollbar...) out of those roles. Apps call widgets, not bb_fillrect.
 *   - apps and shell    only decide WHAT goes WHERE.
 * Pick a theme at build time:   THEME=dark ./build.sh      (default: classic). A theme is just a header, so
 * adding one is "copy classic.h, change the numbers". The `dark` theme exists partly as a test: if anything
 * still shows up in classic colors when it is selected, that is a hard-coded color the refactor missed.
 *
 * Colors are 0xRRGGBB like the COL_* constants in vga.h (the backbuffer is truecolor).
 */

#ifndef MW_THEME_FILE
#define MW_THEME_FILE "ui/themes/classic.h"
#endif
#include MW_THEME_FILE

/* ---- metrics the rest of the kernel has always spelled by these names ---- */
#define TITLEBAR_H    TH_TITLEBAR_H
#define BTN_W         TH_BTN_W
#define BTN_H         TH_BTN_H
#define BTN_GAP       TH_BTN_GAP
#define TASKBAR_H     TH_TASKBAR_H
#define TASKBAR_Y     (VGA_HEIGHT - TASKBAR_H)

#endif
