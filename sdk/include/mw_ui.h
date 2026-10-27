/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_UI_H
#define MW_UI_H

/* mw_ui.h -- a small immediate-mode toolkit: buttons, check boxes, text boxes, lists, progress bars and a message box, drawn in the
 * desktop theme's colours (sys.theme_color: they follow the dark theme too). SDK 2.0.
 *
 *   mw_ui_t ui; mw_ui_init(&ui);
 *   for (;;) {
 *       mw_ui_begin(&ui);                                   // reads the mouse and keyboard once, resets the widget counter
 *       mw_fill_screen(mw_ui_color(MW_TH_FACE));
 *       if (mw_ui_button(&ui, 10, 10, 80, 20, "OK")) break;  // true on the frame the button is clicked
 *       mw_ui_end(&ui);                                     // sys.present()
 *       if (ui.key == MW_KEY_ESC) break;
 *   }
 *
 * "Immediate mode": you call the widgets every frame, in the same order, and they draw themselves and report what happened. Widgets are
 * told apart by the order of the calls (a counter), so do not call them conditionally in a way that changes the order between frames
 * (draw the widget always, or put conditional ones last). Needs ABI 2 (mouse_poll); the colours need ABI 3, and fall back to the
 * Classic theme on an older kernel. */

typedef struct {
    int mx, my, mb;               /* the pointer and the buttons (bit 0 left, bit 1 right) as of this frame */
    int pressed, released;        /* the LEFT button went down / up since the previous frame */
    int key;                      /* the key typed this frame (0 = none) */
    int id;                       /* the widget counter of this frame */
    int active;                   /* the widget being held with the mouse (id), -1 = none */
    int clicked;                  /* on the frame the button is released: the id that was being held, else -1 */
    int focus;                    /* the text box that has the keyboard (id), -1 = none */
    int prev_mb;
} mw_ui_t;

static inline u32 mw_ui_color(int th_id) {
    if (mw_has(3)) return sys.theme_color(th_id);
    static const u32 classic[11] = { MW_FACE, MW_BLACK, MW_ACCENT, MW_WHITE, MW_WHITE, MW_BLACK, MW_WHITE, MW_SHADOW, MW_BLACK, MW_DESKTOP, MW_YELLOW };
    return classic[th_id < 0 || th_id > 10 ? 0 : th_id];
}
static inline void mw_ui_init(mw_ui_t *ui) { mw_memset(ui, 0, sizeof *ui); ui->active = -1; ui->clicked = -1; ui->focus = -1; }
static inline void mw_ui_begin(mw_ui_t *ui) {
    int mx = ui->mx, my = ui->my, mb = 0;
    sys.mouse_poll(&mx, &my, &mb);
    ui->mx = mx; ui->my = my; ui->mb = mb;
    ui->pressed = (mb & 1) && !(ui->prev_mb & 1);
    ui->released = !(mb & 1) && (ui->prev_mb & 1);
    ui->prev_mb = mb;
    ui->key = sys.key_poll();
    ui->id = 0;
    ui->clicked = -1;
    if (ui->released) { ui->clicked = ui->active; ui->active = -1; }
    if (ui->pressed) ui->focus = -1;                       /* a text box takes the focus back when it is clicked (below) */
}
static inline void mw_ui_end(mw_ui_t *ui) { (void)ui; sys.present(); }

/* a raised or sunken 3-D box in the theme colours */
static inline void mw_ui_box(int x, int y, int w, int h, int sunken, u32 fill) {
    u32 hi = mw_ui_color(MW_TH_LIGHT), lo = mw_ui_color(MW_TH_SHADOW), ol = mw_ui_color(MW_TH_OUTLINE);
    sys.fill_rect(x, y, w, h, fill);
    mw_hline(x, y, w, sunken ? lo : hi); mw_vline(x, y, h, sunken ? lo : hi);
    mw_hline(x, y + h - 1, w, sunken ? hi : lo); mw_vline(x + w - 1, y, h, sunken ? hi : lo);
    if (!sunken) { mw_hline(x + 1, y + h - 2, w - 2, ol); mw_vline(x + w - 2, y + 1, h - 2, ol); }
}
static inline int mw_ui_over(const mw_ui_t *ui, int x, int y, int w, int h) { return mw_in_rect(ui->mx, ui->my, x, y, w, h); }
static inline void mw_ui_label(int x, int y, const char *s) { sys.draw_string_ko(x, y, s, mw_ui_color(MW_TH_TEXT)); }

/* 1 on the frame the button is released while the pointer is still over it (it was pressed over it too) */
static inline int mw_ui_button(mw_ui_t *ui, int x, int y, int w, int h, const char *label) {
    int id = ui->id++, over = mw_ui_over(ui, x, y, w, h);
    if (over && ui->pressed) ui->active = id;
    int down = (ui->active == id && (ui->mb & 1) && over);
    mw_ui_box(x, y, w, h, down, mw_ui_color(MW_TH_FACE));
    sys.draw_string_ko(x + (w - sys.text_width(label)) / 2 + down, y + (h - MW_FONT_H) / 2 + down, label, mw_ui_color(MW_TH_TEXT));
    return ui->clicked == id && over;
}
/* a check box with its label to the right; *value toggles; returns 1 when it changed */
static inline int mw_ui_checkbox(mw_ui_t *ui, int x, int y, const char *label, int *value) {
    int id = ui->id++, w = 16 + sys.text_width(label), over = mw_ui_over(ui, x, y, w, 14), changed = 0;
    if (over && ui->pressed) ui->active = id;
    if (ui->clicked == id && over) { *value = !*value; changed = 1; }
    mw_ui_box(x, y, 13, 13, 1, mw_ui_color(MW_TH_FIELD));
    if (*value) { u32 c = mw_ui_color(MW_TH_FIELD_TEXT); mw_line(x + 3, y + 6, x + 5, y + 9, c); mw_line(x + 5, y + 9, x + 10, y + 3, c); mw_line(x + 3, y + 7, x + 5, y + 10, c); mw_line(x + 5, y + 10, x + 10, y + 4, c); }
    sys.draw_string_ko(x + 18, y + 1, label, mw_ui_color(MW_TH_TEXT));
    return changed;
}
/* a one-line text box editing the NUL-terminated string in buf (capacity cap). Click it to type; Backspace deletes; returns 1 when Enter is
 * pressed in it. Only printable ASCII is accepted (key_poll delivers ASCII). */
static inline int mw_ui_textbox(mw_ui_t *ui, int x, int y, int w, char *buf, int cap) {
    int id = ui->id++, over = mw_ui_over(ui, x, y, w, 16), enter = 0;
    if (over && ui->pressed) ui->focus = id;
    int len = mw_strlen(buf);
    if (ui->focus == id && ui->key) {
        if (ui->key == '\b') { if (len > 0) buf[--len] = 0; }
        else if (ui->key == '\n') enter = 1;
        else if (ui->key >= 32 && ui->key < 127 && len + 1 < cap) { buf[len++] = (char)ui->key; buf[len] = 0; }
    }
    mw_ui_box(x, y, w, 16, 1, mw_ui_color(MW_TH_FIELD));
    int room = (w - 8) / MW_FONT_W, skip = len > room ? len - room : 0;            /* show the tail of a long text */
    sys.draw_string_ko(x + 4, y + 2, buf + skip, mw_ui_color(MW_TH_FIELD_TEXT));
    if (ui->focus == id) sys.fill_rect(x + 4 + (len - skip) * MW_FONT_W, y + 2, 2, MW_FONT_H, mw_ui_color(MW_TH_FIELD_TEXT));
    return enter;
}
static inline void mw_ui_progress(int x, int y, int w, int h, int percent) {
    percent = mw_clamp(percent, 0, 100);
    mw_ui_box(x, y, w, h, 1, mw_ui_color(MW_TH_FIELD));
    sys.fill_rect(x + 2, y + 2, (w - 4) * percent / 100, h - 4, mw_ui_color(MW_TH_ACCENT));
}
/* a list of n strings, rows 14 pixels high, with a scroll column (click the arrows). *sel is the selected row (-1 = none), *scroll the first
 * visible one (keep both between frames). Returns 1 when the selection changed. */
static inline int mw_ui_list(mw_ui_t *ui, int x, int y, int w, int h, const char *const *items, int n, int *sel, int *scroll) {
    int id = ui->id++, rows = (h - 4) / 14, changed = 0, bw = 13;
    if (rows < 1) rows = 1;
    int over_list = mw_ui_over(ui, x, y, w - bw, h);
    if (over_list && ui->pressed && ui->active == -1) { int r = *scroll + (ui->my - y - 2) / 14; if (r >= 0 && r < n && r != *sel) { *sel = r; changed = 1; } ui->active = id; }
    if (mw_ui_over(ui, x + w - bw, y, bw, 13) && ui->pressed && *scroll > 0) (*scroll)--;
    if (mw_ui_over(ui, x + w - bw, y + h - 13, bw, 13) && ui->pressed && *scroll + rows < n) (*scroll)++;
    if (*scroll > n - rows) *scroll = n > rows ? n - rows : 0;
    mw_ui_box(x, y, w, h, 1, mw_ui_color(MW_TH_FIELD));
    for (int i = 0; i < rows && *scroll + i < n; i++) {
        int ry = y + 2 + i * 14, on = (*scroll + i == *sel);
        if (on) sys.fill_rect(x + 2, ry, w - bw - 4, 14, mw_ui_color(MW_TH_ACCENT));
        sys.draw_string_ko(x + 4, ry + 1, items[*scroll + i], mw_ui_color(on ? MW_TH_ACCENT_TEXT : MW_TH_FIELD_TEXT));
    }
    mw_ui_box(x + w - bw, y, bw, 13, 0, mw_ui_color(MW_TH_FACE)); mw_ui_box(x + w - bw, y + h - 13, bw, 13, 0, mw_ui_color(MW_TH_FACE));
    u32 ink = mw_ui_color(MW_TH_TEXT);
    for (int k = 0; k < 3; k++) { mw_hline(x + w - bw + 6 - k, y + 4 + k, 1 + 2 * k, ink); mw_hline(x + w - bw + 4 + k, y + h - 10 + k, 5 - 2 * k, ink); }
    return changed;
}

/* A modal message box in the middle of the drawing area; returns when a button is clicked (or Enter / ESC). kind: 0 = OK, 1 = OK/Cancel,
 * 2 = Yes/No. Returns 1 for OK / Yes, 0 for Cancel / No / ESC. The area behind is left as it was drawn. */
static inline int mw_msgbox(mw_ui_t *ui, const char *title, const char *text, int kind) {
    int W = 240, per = (W - 24) / MW_FONT_W, lines = 1, run = 0;
    for (const char *c = text; *c; c++) { if (*c == '\n' || ++run >= per) { lines++; run = 0; } }
    int H = 15 + 14 + lines * (MW_FONT_H + 2) + 34;
    int X = ((int)sys.screen_w - W) / 2, Y = ((int)sys.screen_h - H) / 2;
    for (;;) {
        mw_ui_begin(ui);
        sys.window_frame(X, Y, W, H, title, MWI_WARNING);
        sys.fill_rect(X + 1, Y + 16, W - 2, H - 17, mw_ui_color(MW_TH_FACE));
        mw_text_wrap(X + 12, Y + 24, W - 24, text, mw_ui_color(MW_TH_TEXT));
        int by = Y + H - 28, r = -1;
        if (kind == 0) { if (mw_ui_button(ui, X + W / 2 - 30, by, 60, 20, "OK")) r = 1; }
        else {
            if (mw_ui_button(ui, X + W / 2 - 66, by, 60, 20, kind == 2 ? "Yes" : "OK")) r = 1;
            if (mw_ui_button(ui, X + W / 2 + 6, by, 60, 20, kind == 2 ? "No" : "Cancel")) r = 0;
        }
        mw_ui_end(ui);
        if (ui->key == '\n') r = 1;
        if (ui->key == MW_KEY_ESC) r = kind == 0 ? 1 : 0;
        if (r >= 0) return r;
    }
}

/* ---- files and the clipboard, in one call each ---- */
/* Reads a whole file into a NUL-terminated block from sys.mem_alloc (free it with sys.mem_free); 0 if it cannot be read. *len gets the size. */
static inline char *mw_file_load(const char *path, int *len) {
    int n = sys.file_size(path);
    if (n < 0) return 0;
    char *p = (char *)sys.mem_alloc((u32)n + 1);
    if (!p) return 0;
    int got = n ? sys.file_read(path, p, (u32)n) : 0;
    if (got < 0) { sys.mem_free(p); return 0; }
    p[got] = 0; if (len) *len = got;
    return p;
}
static inline int mw_file_save(const char *path, const void *data, int len) { return sys.file_write(path, data, (u32)len); }
static inline int mw_clip_copy(const char *text) { return mw_has(3) ? sys.clipboard_set(text, (u32)mw_strlen(text)) : 0; }
/* copies the clipboard into buf as a NUL-terminated string; returns its length (0 = empty or cut short to cap-1) */
static inline int mw_clip_paste(char *buf, int cap) {
    if (!mw_has(3) || cap < 1) { if (cap > 0) buf[0] = 0; return 0; }
    int full = sys.clipboard_get(buf, (u32)cap - 1);
    int n = full < cap - 1 ? full : cap - 1;
    buf[n] = 0;
    return n;
}

#endif
