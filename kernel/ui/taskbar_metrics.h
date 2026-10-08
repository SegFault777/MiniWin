#ifndef MW_UI_TASKBAR_METRICS_H
#define MW_UI_TASKBAR_METRICS_H

/* ui/taskbar_metrics.h -- taskbar constants (heights, button sizes) needed before windows are defined.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Taskbar (bottom of screen, Windows-95-ish strip)
 * ============================================================ */

/* The Start button -- bottom-left corner, obviously. Every desktop OS
 * since 1995 has agreed on this location without ever holding a
 * meeting about it. Sized to fit "AM" plus a proper raised bevel. */
#define STARTBTN_X    3
#define STARTBTN_Y    (TASKBAR_Y + 2)
#define STARTBTN_W    34
#define STARTBTN_H    14

/* The minimized-window pill used to hug the taskbar's left edge; now
 * it scoots over to make room for the Start button. Height is still a
 * fixed constant (TASKBTN_H); pill WIDTH is now computed dynamically by
 * taskbar_layout() below since it has to shrink as more windows pile up. */
#define TASKBTN_X     (STARTBTN_X + STARTBTN_W + 5)
#define TASKBTN_Y     (TASKBAR_Y + 2)
#define TASKBTN_H     14

#endif
