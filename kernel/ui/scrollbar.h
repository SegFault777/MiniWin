/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_UI_SCROLLBAR_H
#define MW_UI_SCROLLBAR_H

/* ui/scrollbar.h -- the scroll bar widget: arrow buttons, a track, a proportional thumb you can DRAG, page-up/page-down by
 * clicking the track, and the mouse wheel. One implementation for every window that scrolls (MiniWeb, MiniShell, Notepad,
 * MiniFiles), instead of four slightly different ones.
 *
 * A scroll bar is a view onto a number line: `total` units of content, `page` of them visible, `pos` the first visible
 * one (0 .. total - page). Units are whatever the app scrolls by: pixels for the browser, rows for the shell. An app
 * does not call the drawing and hit-testing functions below; it REGISTERS a target -- two small callbacks, one that
 * reports the bar's geometry and position, one that stores a new position -- and the desktop code does the rest:
 *     sb_register(WIN_ID_WEB, web_sb_geom, web_sb_set, SB_LINE_WEB);
 *     ... draw:   sb_draw(WIN_ID_WEB);
 *     ... input:  sb_press / sb_drag / sb_release / sb_wheel (ui/input.h calls them for every window)
 * The thumb never gets smaller than SB_MIN_THUMB, and the arithmetic stays inside 32 bits (no 64-bit division in this
 * kernel): track*page and (track-thumb)*pos are both far below 2^31 for any window that fits the screen. */

#define SB_W          12
#define SB_ARROW      12
#define SB_MIN_THUMB  10
#define SB_WHEEL_LINES 3                       /* lines per wheel notch */

/* Fills the geometry (the bar's left x, top y, height) and the model; returns 0 if the bar is not shown right now. */
typedef int  (*sb_geom_fn)(int *x, int *y, int *h, int *total, int *page, int *pos);
typedef void (*sb_set_fn)(int pos);

typedef struct { sb_geom_fn geom; sb_set_fn set; int line; } sb_target_t;
#define SB_MAX_TARGETS 16
static sb_target_t sb_targets[SB_MAX_TARGETS];
static int sb_drag_owner = -1, sb_drag_grab = 0;

static inline void sb_register(int owner, sb_geom_fn geom, sb_set_fn set, int line) {
    if (owner < 0 || owner >= SB_MAX_TARGETS) return;
    sb_targets[owner].geom = geom; sb_targets[owner].set = set; sb_targets[owner].line = line > 0 ? line : 1;
}

static inline int sb_max_pos(int total, int page) { return total > page ? total - page : 0; }
static inline int sb_clamp(int pos, int total, int page) { int m = sb_max_pos(total, page); return pos < 0 ? 0 : pos > m ? m : pos; }

/* the thumb inside a bar of height h: its top (absolute y) and height */
static void sb_thumb(int y, int h, int total, int page, int pos, int *ty, int *th) {
    int track = h - 2 * SB_ARROW;
    if (track < 4) track = 4;
    int t = total > 0 && total > page ? (track * page) / total : track;
    if (t < SB_MIN_THUMB) t = SB_MIN_THUMB;
    if (t > track) t = track;
    int room = track - t, max = sb_max_pos(total, page);
    *th = t;
    *ty = y + SB_ARROW + (max > 0 ? (room * pos) / max : 0);
}

static void sb_arrow(int x, int y, int up, int pressed) {
    ui_button(x, y, SB_W, SB_ARROW, "", 0, 0, pressed);
    int cx = x + SB_W / 2, cy = y + SB_ARROW / 2;
    for (int i = 0; i < 4; i++) {
        int w = up ? i : 3 - i, yy = cy - 2 + i;
        for (int k = -w; k <= w; k++) bb_putpixel(cx + k, yy, TH_TEXT);
    }
}

static void sb_draw_at(int x, int y, int h, int total, int page, int pos) {
    int ty, th;
    sb_thumb(y, h, total, page, pos, &ty, &th);
    ui_scrollbar(x, y + SB_ARROW, SB_W, h - 2 * SB_ARROW, ty, th);
    sb_arrow(x, y, 1, 0);
    sb_arrow(x, y + h - SB_ARROW, 0, 0);
}
static void sb_draw(int owner) {
    int x, y, h, total, page, pos;
    if (owner < 0 || owner >= SB_MAX_TARGETS || !sb_targets[owner].geom) return;
    if (!sb_targets[owner].geom(&x, &y, &h, &total, &page, &pos)) return;
    sb_draw_at(x, y, h, total, page, pos);
}

/* A left-button press. 1 = the bar took it (an arrow step, a page jump, or the start of a thumb drag). */
static int sb_press(int owner, int px, int py) {
    int x, y, h, total, page, pos;
    if (owner < 0 || owner >= SB_MAX_TARGETS || !sb_targets[owner].geom || !sb_targets[owner].geom(&x, &y, &h, &total, &page, &pos)) return 0;
    if (!in_rect(px, py, x, y, SB_W, h)) return 0;
    int line = sb_targets[owner].line;
    if (py < y + SB_ARROW)               pos -= line;
    else if (py >= y + h - SB_ARROW)     pos += line;
    else {
        int ty, th;
        sb_thumb(y, h, total, page, pos, &ty, &th);
        if (py < ty)                     pos -= page > line ? page - line : page;       /* the track above the thumb: a page up */
        else if (py >= ty + th)          pos += page > line ? page - line : page;
        else { sb_drag_owner = owner; sb_drag_grab = py - ty; return 1; }             /* on the thumb: start dragging it */
    }
    sb_targets[owner].set(sb_clamp(pos, total, page));
    return 1;
}
/* Called every poll while the button is held: moves the position with the thumb. */
static void sb_drag(int py) {
    if (sb_drag_owner < 0) return;
    int x, y, h, total, page, pos;
    sb_target_t *t = &sb_targets[sb_drag_owner];
    if (!t->geom || !t->geom(&x, &y, &h, &total, &page, &pos)) { sb_drag_owner = -1; return; }
    int ty, th;
    sb_thumb(y, h, total, page, pos, &ty, &th);
    int track = h - 2 * SB_ARROW, room = track - th, max = sb_max_pos(total, page);
    int want = py - sb_drag_grab - (y + SB_ARROW);
    if (want < 0) want = 0;
    if (want > room) want = room;
    t->set(room > 0 ? (want * max) / room : 0);
}
static inline void sb_release(void) { sb_drag_owner = -1; }
static inline int sb_dragging(void) { return sb_drag_owner >= 0; }

/* The wheel over a window: notches > 0 scroll DOWN (towards the end of the content). */
static int sb_wheel(int owner, int notches) {
    int x, y, h, total, page, pos;
    if (owner < 0 || owner >= SB_MAX_TARGETS || !sb_targets[owner].geom || !sb_targets[owner].geom(&x, &y, &h, &total, &page, &pos)) return 0;
    sb_targets[owner].set(sb_clamp(pos + notches * SB_WHEEL_LINES * sb_targets[owner].line, total, page));
    return 1;
}
#endif
