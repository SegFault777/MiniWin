#ifndef MW_APPS_NOTEPAD_DRAW_H
#define MW_APPS_NOTEPAD_DRAW_H

/* apps/notepad/draw.h -- drawing a Notepad window (title bar, menu bar, text area, caret).
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Notepad window drawing
 * ============================================================ */
static void draw_titlebar_buttons(void) {
    int id = active_np->id;
    ui_titlebar_buttons(btn_min_x(), btn_max_x(), btn_close_x(), btn_y(),
                        pressed_btn_kind == BTN_MIN && pressed_btn_win == id,
                        pressed_btn_kind == BTN_MAX && pressed_btn_win == id,
                        pressed_btn_kind == BTN_CLOSE && pressed_btn_win == id);
}

static void draw_window(void) {
    int wx = active_np->win.x, wy = active_np->win.y, ww = active_np->win.w, wh = active_np->win.h;

    /* shadow (not when maximized), body, outline, title bar */
    ui_window_frame(wx, wy, ww, wh, active_np->win.maximized, "NOTEPAD.MWP");

    draw_titlebar_buttons();

    /* menu bar */
    int menu_y = wy + TITLEBAR_H + 1;
    bb_fillrect(wx + 1, menu_y, ww - 2, FONT_CELL + 3, TH_FACE);
    /* highlight the File label while its dropdown is open, like a pressed menu button */
    u32 file_fg = ui_item(wx + 3, menu_y, MENU_FILE_LABEL_W - 2, FONT_CELL + 3, active_np->file_menu_open);
    ko_draw_mixed_string(wx + 5,  menu_y + 2, t(STR_FILE), file_fg);
    ko_draw_mixed_string(wx + 5 + MENU_ITEM_SPACING,     menu_y + 2, t(STR_EDIT), TH_TEXT);
    ko_draw_mixed_string(wx + 5 + MENU_ITEM_SPACING * 2, menu_y + 2, t(STR_HELP), TH_TEXT);
    ui_hline(wx + 1, menu_y + FONT_CELL + 3, ww - 2, TH_SHADOW);

    /* text edit area (white, sunken border) */
    int ex = edit_x(), ey = edit_y(), ew = edit_w(), eh = edit_h();
    ui_well(ex, ey, ew, eh, TH_FIELD);

    /* text content -- UTF-8 aware: most bytes are 1 ASCII char, but a
     * 3-byte sequence (0xE0 lead byte) is one Hangul syllable/jamo drawn
     * via the Korean font instead of the Latin one. Both render at the
     * same FONT_CELL advance since Latin and Hangul glyphs both come out
     * of the same Galmuri11 font at the same 11x11 cell size now. */
    int cx = ex + 2, cy = ey + 2;
    for (u32 i = 0; i < active_np->text_len; ) {
        int clen = ko_utf8_char_len((unsigned char)active_np->text_buf[i]);
        if (clen == 3 && i + 3 <= active_np->text_len) {
            if (cx > ex + ew - FONT_CELL - 2) { cx = ex + 2; cy += FONT_CELL + 1; }
            if (cy > ey + eh - FONT_CELL) break;
            int cp = ko_utf8_decode3(&active_np->text_buf[i]);
            ko_font_draw_codepoint(cx, cy, cp, TH_FIELD_TEXT);
            cx += FONT_CELL;
            i += 3;
            continue;
        }
        char c = active_np->text_buf[i];
        if (c == '\n' || cx > ex + ew - FONT_CELL - 2) {
            cx = ex + 2;
            cy += FONT_CELL + 1;
            if (c == '\n') { i++; continue; }
        }
        if (cy > ey + eh - FONT_CELL) break;
        font_draw_char(cx, cy, c, TH_FIELD_TEXT);
        cx += FONT_CELL;
        i++;
    }

    /* Live preview of the syllable currently being composed (if Hangul
     * mode is on and something's mid-composition), shown right at the
     * cursor position before it's actually committed to active_np->text_buf. */
    if (current_ime == IME_KOREAN && ko_ime_is_composing()) {
        if (cx > ex + ew - FONT_CELL - 2) { cx = ex + 2; cy += FONT_CELL + 1; }
        int preview_cp = ko_ime_preview_codepoint();
        if (preview_cp >= 0 && cy <= ey + eh - FONT_CELL) {
            ko_font_draw_codepoint(cx, cy, preview_cp, TH_FIELD_TEXT);
            cx += FONT_CELL; /* advance past the preview glyph so the cursor bar
                      * below is drawn after it, not overlapping it */
        }
    }

    bb_fillrect(cx, cy, 2, FONT_CELL, TH_FIELD_TEXT); /* text cursor */
}

/* Is (px,py) over the draggable part of the title bar -- i.e. the title
 * bar itself, but not over any of the three control buttons? */
static int titlebar_drag_hit(int px, int py) {
    if (!in_rect(px, py, active_np->win.x + 1, active_np->win.y + 1, active_np->win.w - 2, TITLEBAR_H)) return 0;
    if (in_rect(px, py, btn_min_x(), btn_y(), BTN_W, BTN_H)) return 0;
    if (in_rect(px, py, btn_max_x(), btn_y(), BTN_W, BTN_H)) return 0;
    if (in_rect(px, py, btn_close_x(), btn_y(), BTN_W, BTN_H)) return 0;
    return 1;
}

/* Generic maximize/restore -- works on any window_t, so both Notepad and
 * SETTING.MWP's window can share one implementation instead of two
 * copies of the same four assignments. */
static void maximize_window(window_t *w) {
    if (w->maximized) return;
    w->restore_x = w->x;
    w->restore_y = w->y;
    w->restore_w = w->w;
    w->restore_h = w->h;
    w->x = MAXIMIZED_X;
    w->y = MAXIMIZED_Y;
    w->w = MAXIMIZED_W;
    w->h = MAXIMIZED_H;
    w->maximized = 1;
}

static void unmaximize_window(window_t *w) {
    if (!w->maximized) return;
    w->x = w->restore_x;
    w->y = w->restore_y;
    w->w = w->restore_w;
    w->h = w->restore_h;
    w->maximized = 0;
}

#endif
