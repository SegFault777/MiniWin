/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_UI_CURSOR_H
#define MW_UI_CURSOR_H

/* ui/cursor.h -- the mouse cursor shapes.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

#include "../cursor_data.h"

/* ============================================================
 * Mouse cursor -- the default arrow, plus four directional resize
 * cursors swapped in whenever the pointer is over a window's edge or
 * corner (see compute_hover_resize_zone() above). Since 1.1-alpha-6 the shapes are the icon pack's own cursors
 * (third_party/icon-bundle/Images/Cursor/, CC BY-SA 4.0), turned into cursor_data.h by tools/gen_cursors.py: a black outline
 * around a white fill, so the pointer shows on any background -- the older single-colour shapes vanished on a black or
 * white window. Each shape is an array of row-strings drawn through one generic walker.
 * ============================================================ */
static void draw_cursor_shape(int x, int y, const char *const *rows, int nrows) {
    for (int j = 0; j < nrows; j++) {
        const char *row = rows[j];
        for (int i = 0; row[i]; i++) {
            if (row[i] == 'A') bb_putpixel(x + i, y + j, TH_CURSOR_OUTLINE);
            else if (row[i] == 'B') bb_putpixel(x + i, y + j, TH_CURSOR_FILL);
        }
    }
}

static void draw_cursor(int x, int y) {
    int dummy_win;
    int zone = (resizing_id >= 0) ? resizing_zone : compute_hover_resize_zone(x, y, &dummy_win);

    if (zone == (RESIZE_N | RESIZE_W) || zone == (RESIZE_S | RESIZE_E)) {
        draw_cursor_shape(x - CURSOR_DIAG_NWSE_W / 2, y - CURSOR_DIAG_NWSE_H / 2, cursor_diag_nwse, CURSOR_DIAG_NWSE_H);
    } else if (zone == (RESIZE_N | RESIZE_E) || zone == (RESIZE_S | RESIZE_W)) {
        draw_cursor_shape(x - CURSOR_DIAG_NESW_W / 2, y - CURSOR_DIAG_NESW_H / 2, cursor_diag_nesw, CURSOR_DIAG_NESW_H);
    } else if (zone == RESIZE_W || zone == RESIZE_E) {
        draw_cursor_shape(x - CURSOR_HRESIZE_W / 2, y - CURSOR_HRESIZE_H / 2, cursor_hresize, CURSOR_HRESIZE_H);
    } else if (zone == RESIZE_N || zone == RESIZE_S) {
        draw_cursor_shape(x - CURSOR_VRESIZE_W / 2, y - CURSOR_VRESIZE_H / 2, cursor_vresize, CURSOR_VRESIZE_H);
    } else {
        draw_cursor_shape(x, y, cursor_arrow, CURSOR_ARROW_H);
    }
}

#endif
