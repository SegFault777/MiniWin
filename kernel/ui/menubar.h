/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_UI_MENUBAR_H
#define MW_UI_MENUBAR_H

/* ui/menubar.h -- the menu bar MiniShell, MiniWeb and MiniFiles show under their title bar, and its drop-downs.
 *
 * Notepad has had a menu bar since the beginning, but it is wired to that one window. This one is a widget: an app describes
 * its menus as DATA (a title and some items, each in English and Korean) and gets drawing, hit-testing and the open/close
 * state for free; what each item DOES stays the app's business -- mb_click() just says which (menu, item) was chosen.
 * Every app has menus that fit what it is (the shell's are about the screen and the disks, the browser's about pages and
 * sites, the file manager's about files), not a copy of Notepad's File/Edit/Help.
 *
 * Only one drop-down is open on the whole desktop at a time (mb_owner is the window id that owns it, -1 = none), which
 * is also how it closes itself when a click lands in another window. Mouse only: no keyboard accelerators yet. */

#define MB_H          (FONT_CELL + 4)           /* the bar: text + separator line */
static int mx, my;                              /* the pointer: defined for real (with its start value) in ui/input.h */
#define MB_ITEM_H     15
#define MB_MAX_MENUS  5
#define MB_MAX_ITEMS  8

typedef struct { const char *en, *ko; } mb_txt_t;
typedef struct { mb_txt_t title; int n; mb_txt_t item[MB_MAX_ITEMS]; } mb_menu_t;

static int mb_owner = -1;                       /* window id whose drop-down is open, -1 = none */
static int mb_menu_open = -1;                   /* which of its menus */

static inline const char *mb_str(const mb_txt_t *x) { return (sys_language == LANG_KOREAN && x->ko) ? x->ko : x->en; }
static inline void mb_close(void) { mb_owner = -1; mb_menu_open = -1; }

static inline int mb_title_w(const mb_menu_t *m) { return ko_string_width(mb_str(&m->title)) + 14; }
static inline int mb_title_x(const mb_menu_t *menus, int i, int wx) {
    int x = wx + 3;
    for (int k = 0; k < i; k++) x += mb_title_w(&menus[k]);
    return x;
}
static inline int mb_drop_w(const mb_menu_t *m) {
    int w = 60;
    for (int i = 0; i < m->n; i++) { int iw = ko_string_width(mb_str(&m->item[i])) + 16; if (iw > w) w = iw; }
    return w;
}

/* The bar itself (call right after the title bar; the app's own content starts MB_H lower). */
static void mb_draw_bar(int owner, int wx, int wy, int ww, const mb_menu_t *menus, int n) {
    int y = wy + TITLEBAR_H + 1;
    bb_fillrect(wx + 1, y, ww - 2, MB_H - 1, TH_FACE);
    for (int i = 0; i < n; i++) {
        int open = (mb_owner == owner && mb_menu_open == i);
        int x = mb_title_x(menus, i, wx), w = mb_title_w(&menus[i]);
        u32 fg = ui_item(x, y, w - 2, MB_H - 1, open);
        ko_draw_mixed_string(x + 6, y + 2, mb_str(&menus[i].title), fg);
    }
    ui_hline(wx + 1, y + MB_H - 1, ww - 2, TH_SHADOW);
}
/* The open drop-down, if this window owns it. Draw it LAST so it sits on top of the app's content. */
static void mb_draw_drop(int owner, int wx, int wy, const mb_menu_t *menus, int n) {
    if (mb_owner != owner || mb_menu_open < 0 || mb_menu_open >= n) return;
    const mb_menu_t *m = &menus[mb_menu_open];
    int x = mb_title_x(menus, mb_menu_open, wx), y = wy + TITLEBAR_H + MB_H, w = mb_drop_w(m);
    bb_fillrect(x, y, w, m->n * MB_ITEM_H + 2, TH_FACE);
    bb_rect(x, y, w, m->n * MB_ITEM_H + 2, TH_OUTLINE);
    for (int i = 0; i < m->n; i++) {
        int iy = y + 1 + i * MB_ITEM_H;
        int hover = in_rect(mx, my, x + 1, iy, w - 2, MB_ITEM_H);
        u32 fg = ui_item(x + 1, iy, w - 2, MB_ITEM_H, hover);
        ko_draw_mixed_string(x + 8, iy + 2, mb_str(&m->item[i]), fg);
    }
}

/* A left click at (px,py). Returns 1 if the menu machinery took it (the app should do nothing else with it):
 *   *item_out >= 0  an item was chosen: (*menu_out, *item_out)
 *   *item_out == -1 a menu title was clicked (opened/closed/switched) or the click was swallowed by the bar
 * Returns 0 if the click is not the menu's business -- after closing this window's open drop-down, if there was one. */
static int mb_click(int owner, int wx, int wy, int ww, const mb_menu_t *menus, int n, int px, int py, int *menu_out, int *item_out) {
    *menu_out = -1; *item_out = -1;
    int bar_y = wy + TITLEBAR_H + 1;
    if (mb_owner == owner && mb_menu_open >= 0 && mb_menu_open < n) {              /* a drop-down is open: items first */
        const mb_menu_t *m = &menus[mb_menu_open];
        int x = mb_title_x(menus, mb_menu_open, wx), y = wy + TITLEBAR_H + MB_H, w = mb_drop_w(m);
        for (int i = 0; i < m->n; i++) {
            if (in_rect(px, py, x + 1, y + 1 + i * MB_ITEM_H, w - 2, MB_ITEM_H)) {
                *menu_out = mb_menu_open; *item_out = i;
                mb_close();
                return 1;
            }
        }
    }
    if (in_rect(px, py, wx + 1, bar_y, ww - 2, MB_H)) {                           /* the bar */
        for (int i = 0; i < n; i++) {
            if (in_rect(px, py, mb_title_x(menus, i, wx), bar_y, mb_title_w(&menus[i]), MB_H)) {
                if (mb_owner == owner && mb_menu_open == i) mb_close();
                else { mb_owner = owner; mb_menu_open = i; }
                return 1;
            }
        }
        return 1;                                                                  /* empty part of the bar: swallow */
    }
    if (mb_owner == owner) mb_close();                                             /* a click elsewhere dismisses the drop-down */
    return 0;
}
#endif
