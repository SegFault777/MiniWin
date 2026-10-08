#ifndef MW_APPS_SETTING_H
#define MW_APPS_SETTING_H

/* apps/setting.h -- SETTING.MWP: language / IME / time zone window.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * SETTING.MWP window -- SYSTEM > Language / IME.
 *
 * Reuses window_t, and now (per popular demand) supports minimize and
 * maximize exactly like Notepad's window does -- same three-button
 * title bar, same taskbar-pill-when-minimized treatment, just its own
 * independent state and its own taskbar slot. (setting's window_t
 * itself, and its default-geometry constants, live up near notepad's
 * declaration -- see the comment there for why.)
 * ============================================================ */
#define SETTING_SIDEBAR_W   100  /* fits "TIMEZONE" (8 glyphs @ FONT_CELL) plus margin */
#define SETTING_NAV_ITEM_H  16
#define SETTING_ROW_W       150
#define SETTING_ROW_H       16
#define SETTING_ROW_GAP     20

#define SETTING_NAV_LANGUAGE 0
#define SETTING_NAV_IME      1
#define SETTING_NAV_TIMEZONE 2

/* Which sidebar page is showing. Persists across close/reopen within
 * the same boot, same as any real settings app remembering your last
 * tab -- nobody wants to re-navigate to Language every single time. */
static int setting_page = SETTING_NAV_LANGUAGE;

static inline int setting_btn_close_x(void) { return win_btn_close_x(&setting, 3); }
static inline int setting_btn_max_x(void)   { return win_btn_max_x(&setting, 3); }
static inline int setting_btn_min_x(void)   { return win_btn_min_x(&setting, 3); }
static inline int setting_btn_y(void)       { return win_btn_y(&setting, 2); }

static int setting_close_hit(int px, int py) {
    return in_rect(px, py, setting_btn_close_x(), setting_btn_y(), BTN_W, BTN_H);
}
static int setting_min_hit(int px, int py) {
    return in_rect(px, py, setting_btn_min_x(), setting_btn_y(), BTN_W, BTN_H);
}
static int setting_max_hit(int px, int py) {
    return in_rect(px, py, setting_btn_max_x(), setting_btn_y(), BTN_W, BTN_H);
}

/* Draggable part of the title bar -- the whole bar except its three
 * buttons, same idea as Notepad's titlebar_drag_hit(). */
static int setting_titlebar_drag_hit(int px, int py) {
    if (!in_rect(px, py, setting.x + 1, setting.y + 1, setting.w - 2, TITLEBAR_H)) return 0;
    if (in_rect(px, py, setting_btn_min_x(), setting_btn_y(), BTN_W, BTN_H)) return 0;
    if (in_rect(px, py, setting_btn_max_x(), setting_btn_y(), BTN_W, BTN_H)) return 0;
    if (in_rect(px, py, setting_btn_close_x(), setting_btn_y(), BTN_W, BTN_H)) return 0;
    return 1;
}

static inline int setting_header_y(void) { return setting.y + TITLEBAR_H + 4; }
static inline int setting_nav_y(int idx) { return setting_header_y() + 14 + idx * SETTING_NAV_ITEM_H; }

static int setting_nav_hit(int px, int py, int idx) {
    return in_rect(px, py, setting.x + 3, setting_nav_y(idx), SETTING_SIDEBAR_W - 4, SETTING_NAV_ITEM_H);
}

static inline int setting_content_x(void) { return setting.x + SETTING_SIDEBAR_W + 5; }
static inline int setting_row_y(int idx) { return setting_header_y() + idx * SETTING_ROW_GAP; }

static int setting_row_hit(int px, int py, int idx) {
    return in_rect(px, py, setting_content_x(), setting_row_y(idx), SETTING_ROW_W, SETTING_ROW_H);
}

/* Time Zone page: "UTC+9  [-] [+]" -- two small buttons next to the
 * current offset, rather than a text field this kernel has no widget
 * for. Bounded to a plausible +/-14 range (the real-world extremes,
 * roughly) when clicked. */
#define TZ_BTN_W 17
#define TZ_BTN_H 16
static inline int tz_minus_x(void) { return setting_content_x() + 72; }
static inline int tz_plus_x(void)  { return setting_content_x() + 94; }
static inline int tz_btn_y(void)   { return setting_row_y(0) - 1; }
static int tz_minus_hit(int px, int py) { return in_rect(px, py, tz_minus_x(), tz_btn_y(), TZ_BTN_W, TZ_BTN_H); }
static int tz_plus_hit(int px, int py)  { return in_rect(px, py, tz_plus_x(),  tz_btn_y(), TZ_BTN_W, TZ_BTN_H); }

/* Builds "(*) Name" / "( ) Name" (radio, single-select -- Language) or
 * "<x> Name" / "< > Name" (checkbox, multi-select -- IME) into `out`.
 * `name` is deliberately each language's OWN name for itself
 * ("English", "한국어") rather than anything from the ui_strings
 * table -- exactly how every real language picker does it, so you can
 * still find your language even if you can't read whichever one is
 * currently active. */
static void build_option_label(char *out, u32 outsz, int is_radio, int selected, const char *name) {
    u32 len = 0;
    /* Angle brackets, not square ones -- purely a style choice now that
     * the font covers full printable ASCII (including '[' / ']') again;
     * this used to be a functional necessity back when the old 8x8 font
     * only covered up through 'Z' and square brackets would've silently
     * drawn as blank gaps. Kept as angle brackets anyway because "<x>"
     * still reads as visually distinct from the radio buttons' "(*)",
     * which is the whole point of using different bracket styles for
     * single- vs multi-select. */
    const char *pre = is_radio ? (selected ? "(*) " : "( ) ")
                                : (selected ? "<x> " : "< > ");
    while (*pre) kstrcpy_append(out, &len, outsz, *pre++);
    while (*name) kstrcpy_append(out, &len, outsz, *name++);
}

static void draw_setting_window(void) {
    int wx = setting.x, wy = setting.y, ww = setting.w, wh = setting.h;

    ui_window_frame(wx, wy, ww, wh, setting.maximized, "SETTING.MWP");

    /* the three title-bar buttons (this window keeps its own 3px/2px inset -- see setting_btn_*()) */
    ui_titlebar_buttons(setting_btn_min_x(), setting_btn_max_x(), setting_btn_close_x(), setting_btn_y(),
                        pressed_btn_kind == BTN_MIN && pressed_btn_win == WIN_ID_SETTING,
                        pressed_btn_kind == BTN_MAX && pressed_btn_win == WIN_ID_SETTING,
                        pressed_btn_kind == BTN_CLOSE && pressed_btn_win == WIN_ID_SETTING);

    /* sidebar / content divider */
    int body_y = wy + TITLEBAR_H + 1;
    int body_h = wh - TITLEBAR_H - 2;
    ui_vline(wx + SETTING_SIDEBAR_W, body_y, body_h, TH_SHADOW);

    /* "SYSTEM" section header, then the three navigable pages under it */
    ko_draw_mixed_string(wx + 3, body_y + 2, t(STR_SYSTEM), TH_TEXT);
    const char *nav_labels[3] = { t(STR_LANGUAGE), t(STR_IME), t(STR_TIMEZONE) };
    for (int i = 0; i < 3; i++) {
        int ny = setting_nav_y(i);
        int active = (setting_page == i);
        u32 fg = ui_item(wx + 2, ny, SETTING_SIDEBAR_W - 3, SETTING_NAV_ITEM_H, active);
        ko_draw_mixed_string(wx + 6, ny + 1, nav_labels[i], fg);
    }

    /* content area for whichever page is active */
    char label[24];
    if (setting_page == SETTING_NAV_LANGUAGE) {
        build_option_label(label, sizeof(label), 1, sys_language == LANG_ENGLISH, "English");
        ko_draw_mixed_string(setting_content_x(), setting_row_y(0), label, TH_TEXT);
        build_option_label(label, sizeof(label), 1, sys_language == LANG_KOREAN, "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4");
        ko_draw_mixed_string(setting_content_x(), setting_row_y(1), label, TH_TEXT);
    } else if (setting_page == SETTING_NAV_IME) {
        build_option_label(label, sizeof(label), 0, ime_enabled[IME_ENGLISH], "English");
        ko_draw_mixed_string(setting_content_x(), setting_row_y(0), label, TH_TEXT);
        build_option_label(label, sizeof(label), 0, ime_enabled[IME_KOREAN], "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4");
        ko_draw_mixed_string(setting_content_x(), setting_row_y(1), label, TH_TEXT);
    } else {
        build_tz_label(label, sizeof(label));
        ko_draw_mixed_string(setting_content_x(), setting_row_y(0) + 1, label, TH_TEXT);

        int mnx = tz_minus_x(), mxx = tz_plus_x(), by = tz_btn_y();
        ui_panel(mnx, by, TZ_BTN_W, TZ_BTN_H, 0);
        font_draw_string(mnx + 3, by + 1, "-", TH_TEXT);
        ui_panel(mxx, by, TZ_BTN_W, TZ_BTN_H, 0);
        font_draw_string(mxx + 3, by + 1, "+", TH_TEXT);
    }
}

#endif
