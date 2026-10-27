/* SPDX-License-Identifier: GPL-3.0-only */
/* browse.c -- a tiny folder browser built only from the SDK: dir_list, file_size, icons, mem_alloc, keyboard.
 * Up/Down (w/s) move, Enter (or d) opens a folder, Backspace (or a) goes up, ESC quits. */
#include "miniwin.h"

#define MAXE 64
typedef struct { char name[32]; int is_dir; u32 size; } entry_t;
typedef struct { entry_t *e; int n; } list_t;

static int collect(const char *name, int is_dir, u32 size, void *ctx) {
    list_t *l = (list_t *)ctx;
    if (l->n >= MAXE) return 0;
    entry_t *x = &l->e[l->n++];
    int i = 0; while (name[i] && i < 31) { x->name[i] = name[i]; i++; }
    x->name[i] = 0; x->is_dir = is_dir; x->size = size;
    return 1;
}

void mwp_entry(void) {
    if (!mw_has(2)) return;
    list_t l; l.e = sys.mem_alloc(sizeof(entry_t) * MAXE); l.n = 0;
    char *path = sys.mem_alloc(96);
    if (!l.e || !path) return;
    path[0] = '/'; path[1] = 0;
    int sel = 0, top = 0, dirty = 1;
    int win = mw_has(3) && mw_windowed();
    for (;;) {
        int W = win ? (int)sys.screen_w : 420, H = win ? (int)sys.screen_h : 260, X = win ? 0 : ((int)sys.screen_w - W) / 2, Y = win ? -17 : ((int)sys.screen_h - H) / 2;
        int ROWS = (H - 62) / 16;          /* as many rows as the area has room for (re-computed: the window can be resized) */
        if (ROWS < 1) ROWS = 1;
        if (dirty) { l.n = 0; sys.dir_list(path, collect, &l); sel = 0; top = 0; dirty = 0; }
        if (win) mw_fill_screen(MW_FACE);
        else {
            mw_fill_screen(MW_DESKTOP);
            sys.window_frame(X, Y, W, H, "BROWSE.MWP", MWI_FILES);
            sys.fill_rect(X + 1, Y + 17, W - 2, H - 18, MW_FACE);
        }
        sys.bevel(X + 6, Y + 22, W - 12, 18, 1);
        mw_text(X + 10, Y + 25, path, MW_BLACK);
        sys.bevel(X + 6, Y + 44, W - 12, H - 52, 1);
        sys.fill_rect(X + 8, Y + 46, W - 16, H - 56, MW_WHITE);
        if (sel < top) top = sel;
        if (sel >= top + ROWS) top = sel - ROWS + 1;
        for (int i = 0; i < ROWS && top + i < l.n; i++) {
            entry_t *e = &l.e[top + i]; int ry = Y + 48 + i * 16;
            u32 fg = MW_BLACK;
            if (top + i == sel) { sys.fill_rect(X + 28, ry, W - 40, 15, MW_ACCENT); fg = MW_WHITE; }
            sys.draw_icon16(X + 10, ry, e->is_dir ? MWI_FOLDER : MWI_DOC);
            mw_text(X + 30, ry + 2, e->name, fg);
            if (!e->is_dir) { char t[12]; int n = 0; char s[16]; s[0] = 0; mw_append(s, &n, mw_itoa((int)e->size, t)); mw_append(s, &n, " B"); mw_text(X + W - 12 - sys.text_width(s), ry + 2, s, fg); }
        }
        if (l.n == 0) mw_text(X + 14, Y + 52, "(empty)", MW_SHADOW);
        sys.present();
        int k = sys.key_poll();
        if (k == MW_KEY_ESC) break;
        else if ((k == 'w' || k == 'W') && sel > 0) sel--;
        else if ((k == 's' || k == 'S') && sel < l.n - 1) sel++;
        else if ((k == '\n' || k == 'd') && l.n && l.e[sel].is_dir) {
            int n = mw_str_len(path);
            if (!(n == 1 && path[0] == '/')) path[n++] = '/';
            for (const char *c = l.e[sel].name; *c && n < 94; c++) path[n++] = *c;
            path[n] = 0; dirty = 1;
        } else if (k == MW_KEY_BACKSPACE || k == 'a') {
            int n = mw_str_len(path);
            while (n > 1 && path[n - 1] != '/') n--;
            if (n > 1) n--;
            path[n] = 0; if (!path[0]) { path[0] = '/'; path[1] = 0; } dirty = 1;
        }
    }
    sys.mem_free(l.e); sys.mem_free(path);
}
