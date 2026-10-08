#ifndef MW_UI_WIDGETS_H
#define MW_UI_WIDGETS_H

/* ui/widgets.h -- every reusable piece of chrome, drawn from theme roles (ui/theme.h).
 *
 * These were previously copy-pasted per window (the three title-bar buttons alone existed in four copies).
 * Each function here reproduces the exact pixels the copies drew, so the refactor changed nothing on screen
 * (tools/test/ui_golden.sh proves it). Rules of the house:
 *   - widgets only DRAW. They keep no state and make no decisions; hover/pressed/focus arrive as arguments.
 *   - geometry (where is the button?) stays with the window that owns it; widgets take plain rectangles.
 *   - a widget that picks a text color for you returns it (ui_item, ui_button), so the caller never has to
 *     remember which text color goes on which fill.
 * Requires: vga.h (bb_*), font.h, theme.h.
 */

/* ---- lines ---- */
static inline void ui_hline(int x, int y, int w, u32 c) { for (int i = 0; i < w; i++) bb_putpixel(x + i, y, c); }
static inline void ui_vline(int x, int y, int h, u32 c) { for (int j = 0; j < h; j++) bb_putpixel(x, y + j, c); }

/* ---- surfaces ---- */
/* A raised (or, while pressed, sunken) 3D button face: light top-left / dark bottom-right, flipped while pressed.
 * The interior is left clean for a label or glyph (callers nudge it +1,+1 while pressed so it looks like it sank). */
static void ui_bevel(int x, int y, int w, int h, int pressed) {
    bb_fillrect(x, y, w, h, TH_FACE);
    u32 hi = pressed ? TH_SHADOW : TH_LIGHT;
    u32 lo = pressed ? TH_LIGHT : TH_SHADOW;
    for (int i = 0; i < w - 1; i++) bb_putpixel(x + i, y, hi);
    for (int j = 0; j < h - 1; j++) bb_putpixel(x, y + j, hi);
    for (int i = 0; i < w; i++) bb_putpixel(x + i, y + h - 1, lo);
    for (int j = 0; j < h; j++) bb_putpixel(x + w - 1, y + j, lo);
}

/* A framed panel: optional drop shadow (offset `shadow`, 0 = none), TH_FACE body, TH_OUTLINE frame.
 * Menus, dialogs, popups and windows are all this. */
static void ui_panel(int x, int y, int w, int h, int shadow) {
    if (shadow) bb_fillrect(x + shadow, y + shadow, w, h, TH_SHADOW);
    bb_fillrect(x, y, w, h, TH_FACE);
    bb_rect(x, y, w, h, TH_OUTLINE);
}

/* The accent-colored title strip inside a panel's frame, with its title text. */
static void ui_titlebar(int x, int y, int w, const char *title) {
    bb_fillrect(x + 1, y + 1, w - 2, TITLEBAR_H, TH_ACCENT);
    font_draw_string(x + 3, y + 1, title, TH_ACCENT_TEXT);
}

/* A whole window frame: shadow (not when maximized -- it would hang off the screen edge), body, outline, title bar. */
static void ui_window_frame(int x, int y, int w, int h, int maximized, const char *title) {
    ui_panel(x, y, w, h, maximized ? 0 : TH_WINDOW_SHADOW);
    ui_titlebar(x, y, w, title);
}

/* A sunken, typeable field: `fill` background, a 1px frame that is TH_SHADOW (or TH_ACCENT when focused) on the
 * top/left and TH_FACE on the bottom/right. (Used where the frame sits INSIDE the rectangle.) */
static void ui_field(int x, int y, int w, int h, u32 fill, int focused) {
    bb_fillrect(x, y, w, h, fill);
    u32 border = focused ? TH_ACCENT : TH_SHADOW;
    ui_hline(x, y, w, border);
    ui_vline(x, y, h, border);
    ui_hline(x, y + h - 1, w, TH_FACE);
    ui_vline(x + w - 1, y, h, TH_FACE);
}

/* A "well": the rectangle is filled, and a TH_SHADOW outline is drawn just OUTSIDE it (Notepad's edit area). */
static void ui_well(int x, int y, int w, int h, u32 fill) {
    bb_fillrect(x, y, w, h, fill);
    bb_rect(x - 1, y - 1, w + 2, h + 2, TH_SHADOW);
}

/* The little inset readout (the taskbar clock): a 1px frame, dark top/left when idle, flipped while `active`. */
static void ui_readout(int x, int y, int w, int h, int active) {
    bb_fillrect(x, y, w, h, TH_FACE);
    bb_rect(x, y, w, h, active ? TH_LIGHT : TH_SHADOW);
    bb_putpixel(x, y, active ? TH_SHADOW : TH_LIGHT);
}

/* A taskbar pill: field-colored with a TH_SHADOW outline (the minimized-window buttons). */
static void ui_pill(int x, int y, int w, int h) {
    bb_fillrect(x, y, w, h, TH_FIELD);
    bb_rect(x, y, w, h, TH_SHADOW);
}

/* ---- selectable things ---- */
/* A row in a menu / list / sidebar: accent fill with inverse text when `selected` (hovered or current),
 * face fill with normal text otherwise. Returns the text color to draw the label with. */
static u32 ui_item(int x, int y, int w, int h, int selected) {
    bb_fillrect(x, y, w, h, selected ? TH_ACCENT : TH_FACE);
    return selected ? TH_TEXT_INVERSE : TH_TEXT;
}

/* A flat outlined button (the confirm dialog's Yes/No): field-colored, accent on hover. Returns the label color. */
static u32 ui_button_flat(int x, int y, int w, int h, int hover) {
    bb_fillrect(x, y, w, h, hover ? TH_ACCENT : TH_FIELD);
    bb_rect(x, y, w, h, TH_OUTLINE);
    return hover ? TH_TEXT_INVERSE : TH_FIELD_TEXT;
}

/* A labelled bevel button; the label sinks by one pixel while pressed. */
static void ui_button(int x, int y, int w, int h, const char *label, int label_dx, int label_dy, int pressed) {
    ui_bevel(x, y, w, h, pressed);
    int o = pressed ? 1 : 0;
    font_draw_string(x + label_dx + o, y + label_dy + o, label, TH_TEXT);
}

/* ---- title-bar glyphs (each draws its own bevel; `pressed` sinks both bevel and glyph) ---- */
static void ui_glyph_minimize(int x, int y, int pressed) {
    ui_bevel(x, y, BTN_W, BTN_H, pressed);
    int o = pressed ? 1 : 0;
    for (int i = 2; i < BTN_W - 2; i++) bb_putpixel(x + i + o, y + BTN_H - 3 + o, TH_GLYPH);
}
static void ui_glyph_maximize(int x, int y, int pressed) {
    ui_bevel(x, y, BTN_W, BTN_H, pressed);
    int o = pressed ? 1 : 0;
    bb_rect(x + 2 + o, y + 2 + o, BTN_W - 4, BTN_H - 4, TH_GLYPH);
}
/* `top` is the row (from y) where the X starts: 2 on windows, 1 on the dialog's slightly higher X. */
static void ui_glyph_close(int x, int y, int pressed, int top) {
    ui_bevel(x, y, BTN_W, BTN_H, pressed);
    int o = pressed ? 1 : 0;
    for (int i = 2; i < BTN_W - 2; i++) {
        bb_putpixel(x + i + o, y + top + (i - 2) + o, TH_GLYPH);
        bb_putpixel(x + (BTN_W - 1 - i) + o, y + top + (i - 2) + o, TH_GLYPH);
    }
}
/* The standard _ [] X trio, laid out by the caller's geometry. */
static void ui_titlebar_buttons(int min_x, int max_x, int close_x, int y, int p_min, int p_max, int p_close) {
    ui_glyph_minimize(min_x, y, p_min);
    ui_glyph_maximize(max_x, y, p_max);
    ui_glyph_close(close_x, y, p_close, 2);
}

/* ---- scrollbar: a track with a proportional thumb (the caller computes the thumb's place) ---- */
static void ui_scrollbar(int x, int y, int w, int h, int thumb_y, int thumb_h) {
    bb_fillrect(x, y, w, h, TH_SCROLL_TRACK);
    bb_fillrect(x + 1, thumb_y, w - 2, thumb_h, TH_SCROLL_THUMB);
}

#endif
