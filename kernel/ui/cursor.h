#ifndef MW_UI_CURSOR_H
#define MW_UI_CURSOR_H

/* ui/cursor.h -- the mouse cursor shapes.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Mouse cursor -- the default arrow, plus four directional resize
 * cursors swapped in whenever the pointer is over a window's edge or
 * corner (see compute_hover_resize_zone() above). Each shape is just a
 * small array of row-strings, same pixel-art-by-hand approach as every
 * other icon in this OS, drawn through one generic walker instead of a
 * separate function per shape.
 * ============================================================ */
static const char *cursor_arrow[11] = {
    "X......","XX.....","X.X....","X..X...","X...X..",
    "X....X.","X.....X","X....XX","X..X.X.","X.X..X.","XX...X.",
};
static const char *cursor_hresize[5] = {
    "...X...X...",
    "..XX...XX..",
    ".XXXXXXXXX.",
    "..XX...XX..",
    "...X...X...",
};
static const char *cursor_vresize[11] = {
    "..X..",".XXX.","XXXXX","..X..","..X..",
    "..X..","..X..","..X..","XXXXX",".XXX.","..X..",
};
/* top-left <-> bottom-right ("\") */
static const char *cursor_diag_nwse[9] = {
    "XX.......","XXX......",".XXX.....","..XXX....","...XXX...",
    "....XXX..",".....XXX.","......XXX",".......XX",
};
/* top-right <-> bottom-left ("/") */
static const char *cursor_diag_nesw[9] = {
    ".......XX","......XXX",".....XXX.","....XXX..","...XXX...",
    "..XXX....",".XXX.....","XXX......","XX.......",
};

static void draw_cursor_shape(int x, int y, const char *const *rows, int nrows) {
    for (int j = 0; j < nrows; j++) {
        const char *row = rows[j];
        for (int i = 0; row[i]; i++) {
            if (row[i] == 'X') bb_putpixel(x + i, y + j, TH_CURSOR);
        }
    }
}

static void draw_cursor(int x, int y) {
    int dummy_win;
    int zone = (resizing_id >= 0) ? resizing_zone : compute_hover_resize_zone(x, y, &dummy_win);

    if (zone == (RESIZE_N | RESIZE_W) || zone == (RESIZE_S | RESIZE_E)) {
        draw_cursor_shape(x - 4, y - 4, cursor_diag_nwse, 9);
    } else if (zone == (RESIZE_N | RESIZE_E) || zone == (RESIZE_S | RESIZE_W)) {
        draw_cursor_shape(x - 4, y - 4, cursor_diag_nesw, 9);
    } else if (zone == RESIZE_W || zone == RESIZE_E) {
        draw_cursor_shape(x - 5, y - 2, cursor_hresize, 5);
    } else if (zone == RESIZE_N || zone == RESIZE_S) {
        draw_cursor_shape(x - 2, y - 5, cursor_vresize, 11);
    } else {
        draw_cursor_shape(x, y, cursor_arrow, 11);
    }
}

#endif
