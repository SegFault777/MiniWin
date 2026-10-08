#ifndef MW_UI_RESIZE_H
#define MW_UI_RESIZE_H

/* ui/resize.h -- window edge/corner resize hit-testing.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Window edge/corner resizing
 *
 * A thin margin straddling each window's border acts as a grab zone --
 * hovering it swaps the cursor to the matching directional arrow, and
 * pressing there starts a resize instead of a drag or a content click.
 * Corners resize two edges at once; maximized windows aren't resizable
 * at all (same as real window managers -- there's nothing to resize
 * when a window is already filling all the space it can).
 * ============================================================ */
#define RESIZE_MARGIN 4

#define RESIZE_NONE 0
#define RESIZE_N    1
#define RESIZE_S    2
#define RESIZE_E    4
#define RESIZE_W    8

/* Which edges (if any) of THIS window's rect are under (px,py), given
 * the window's own current geometry -- corners come back as two bits
 * set together (e.g. RESIZE_N|RESIZE_W for the top-left corner). */
static int resize_zone_at(int px, int py, int x, int y, int w, int h) {
    int within_x = (px >= x - RESIZE_MARGIN && px <= x + w + RESIZE_MARGIN);
    int within_y = (py >= y - RESIZE_MARGIN && py <= y + h + RESIZE_MARGIN);
    int on_top    = within_x && py >= y - RESIZE_MARGIN     && py <= y + RESIZE_MARGIN;
    int on_bottom = within_x && py >= y + h - RESIZE_MARGIN && py <= y + h + RESIZE_MARGIN;
    int on_left   = within_y && px >= x - RESIZE_MARGIN     && px <= x + RESIZE_MARGIN;
    int on_right  = within_y && px >= x + w - RESIZE_MARGIN && px <= x + w + RESIZE_MARGIN;

    int zone = RESIZE_NONE;
    if (on_top) zone |= RESIZE_N;
    if (on_bottom) zone |= RESIZE_S;
    if (on_left) zone |= RESIZE_W;
    if (on_right) zone |= RESIZE_E;
    return zone;
}

/* Walks z-order front-to-back (same priority as click hit-testing) and
 * returns the resize zone under (px,py) for whichever window is
 * topmost there, plus that window's id via *out_win_id. A maximized
 * window still "claims" the point (so clicks there don't fall through
 * to whatever's behind it) but always reports RESIZE_NONE, since it
 * can't be resized while maximized. */
static int compute_hover_resize_zone(int px, int py, int *out_win_id) {
    for (int zi = z_count - 1; zi >= 0; zi--) {
        int id = z_order[zi];
        if (!win_is_open(id) || win_is_minimized(id)) continue;
        window_t *w = win_ptr(id);
        int ex = w->x - RESIZE_MARGIN, ey = w->y - RESIZE_MARGIN;
        int ew = w->w + 2 * RESIZE_MARGIN, eh = w->h + 2 * RESIZE_MARGIN;
        if (!in_rect(px, py, ex, ey, ew, eh)) continue;
        *out_win_id = id;
        if (w->maximized) return RESIZE_NONE;
        return resize_zone_at(px, py, w->x, w->y, w->w, w->h);
    }
    *out_win_id = -1;
    return RESIZE_NONE;
}

/* Active resize, if any -- one shared mechanism for every window, same
 * pattern as dragging_id. */
static int resizing_id = -1;
static int resizing_zone = RESIZE_NONE;
static int resize_start_mx, resize_start_my;
static int resize_start_x, resize_start_y, resize_start_w, resize_start_h;

#endif
