/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_UI_SCROLL_TARGETS_H
#define MW_UI_SCROLL_TARGETS_H

/* ui/scroll_targets.h -- the wiring between the scroll bar widget (ui/scrollbar.h) and the windows that scroll. Each window
 * contributes two tiny callbacks -- "where is your bar and what does it show" and "this is the new position" -- and
 * sb_register_all() hands them to the widget. Nothing else about scrolling lives in the apps: not the arrows, the thumb,
 * the dragging or the wheel. Included after every app (it needs their state) and before sys/boot.h (which registers). */

/* ---- MiniWeb: pixels ---- */
static int sbt_web_geom(int *x, int *y, int *h, int *total, int *page, int *pos) {
    if (!web_win.open || web_win.minimized) return 0;
    int bx, by, bw, bh;
    web_body_geom(&bx, &by, &bw, &bh);
    *x = bx + bw; *y = by; *h = bh;
    *total = web_doc_h > 0 ? web_doc_h : 1; *page = bh; *pos = web_scroll_px;
    return 1;
}
static void sbt_web_set(int p) { web_scroll_px = p; }

/* ---- MiniShell: rows. The terminal counts its scroll back from the NEWEST line, a scroll bar from the top: converted here ---- */
static int sbt_term_total(void) { return term_scroll_count > TERM_VISIBLE_ROWS ? term_scroll_count : TERM_VISIBLE_ROWS; }
static int sbt_term_geom(int *x, int *y, int *h, int *total, int *page, int *pos) {
    if (!term_win.open || term_win.minimized) return 0;
    *x = term_win.x + term_win.w - 3 - SB_W; *y = term_output_y(); *h = term_output_h();
    *total = sbt_term_total(); *page = TERM_VISIBLE_ROWS; *pos = *total - *page - term_scroll_off;
    return 1;
}
static void sbt_term_set(int p) { term_scroll_off = sbt_term_total() - TERM_VISIBLE_ROWS - p; }

/* ---- MiniFiles: rows ---- */
static int sbt_files_geom(int *x, int *y, int *h, int *total, int *page, int *pos) {
    if (!files_win.open || files_win.minimized || files_row_count <= files_visible_rows()) return 0;
    *x = files_list_x() + files_list_w() - SB_W - 1; *y = files_list_y() + 1; *h = files_list_h() - 2;
    *total = files_row_count; *page = files_visible_rows(); *pos = files_scroll;
    return 1;
}
static void sbt_files_set(int p) { files_scroll = p; }

/* ---- Notepad: wrapped text rows, one pair of callbacks per window (the callbacks carry no window number) ---- */
#define SBT_NP(n)                                                                                                        \
static int sbt_np##n##_geom(int *x, int *y, int *h, int *total, int *page, int *pos) {                                  \
    notepad_t *np = &notepads[n];                                                                                        \
    if (!np->win.open || np->win.minimized || np->confirm_mode != CONFIRM_NONE) return 0;                                \
    *x = np->win.x + np->win.w - 5 - SB_W; *y = np->win.y + TITLEBAR_H + FONT_CELL + 6;                                  \
    *h = np->win.h - TITLEBAR_H - FONT_CELL - 10;                                                                        \
    *total = np_rows(np, np->win.w - 10); *page = np_visible_rows(np->win.h - TITLEBAR_H - FONT_CELL - 10); *pos = np->scroll; \
    return 1;                                                                                                            \
}                                                                                                                        \
static void sbt_np##n##_set(int p) { notepads[n].scroll = p; }
SBT_NP(0) SBT_NP(1) SBT_NP(2) SBT_NP(3)
typedef char sbt_assert_four_notepads[NOTEPAD_MAX == 4 ? 1 : -1];

static void sb_register_all(void) {
    sb_register(WIN_ID_WEB,      sbt_web_geom,   sbt_web_set,   14);        /* a line of body text, in pixels */
    sb_register(WIN_ID_TERMINAL, sbt_term_geom,  sbt_term_set,  1);
    sb_register(WIN_ID_FILES,    sbt_files_geom, sbt_files_set, 1);
    sb_register(0, sbt_np0_geom, sbt_np0_set, 1);
    sb_register(1, sbt_np1_geom, sbt_np1_set, 1);
    sb_register(2, sbt_np2_geom, sbt_np2_set, 1);
    sb_register(3, sbt_np3_geom, sbt_np3_set, 1);
}
#endif
