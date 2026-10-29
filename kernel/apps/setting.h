/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_APPS_SETTING_H
#define MW_APPS_SETTING_H

/* apps/setting.h -- SETTING.MWP: language / IME / time zone / display / mouse / sound / text / about / power.
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
#define SETTING_SIDEBAR_W   120  /* fits "TIME ZONE" (9 glyphs @ FONT_CELL) plus margin and the nav scroll bar */
#define SETTING_NAV_ITEM_H  16
#define SETTING_ROW_H       16
#define SETTING_ROW_GAP     18
#define SB_OWNER_SETTING_NAV 14  /* the sidebar's own scroll bar (the content's are registered under WIN_ID_SETTING); 15 is MiniFiles' tree */

/* The pages. (1.1-alpha-12: three became nine. Every line on every page changes something real, immediately; nothing is stored
 * across a reboot yet -- like the language, they start from the defaults each boot.) */
enum {
    SETTING_NAV_LANGUAGE, SETTING_NAV_IME, SETTING_NAV_TIMEZONE, SETTING_NAV_DISPLAY, SETTING_NAV_MOUSE,
    SETTING_NAV_SOUND, SETTING_NAV_TEXT, SETTING_NAV_ABOUT, SETTING_NAV_POWER, SETTING_NAV_COUNT
};
static const mb_txt_t setting_nav_txt[SETTING_NAV_COUNT] = {
    { "LANGUAGE", "언어" }, { "IME", "입력기" }, { "TIME ZONE", "시간대" }, { "DISPLAY", "화면" }, { "MOUSE", "마우스" },
    { "SOUND", "소리" }, { "TEXT", "글자" }, { "ABOUT", "정보" }, { "POWER", "전원" },
};
#define SKO(en, ko) (sys_language == LANG_KOREAN ? (ko) : (en))

/* Which sidebar page is showing. Persists across close/reopen within
 * the same boot, same as any real settings app remembering your last
 * tab -- nobody wants to re-navigate to Language every single time. */
static int setting_page = SETTING_NAV_LANGUAGE;
static int setting_nav_scroll = 0;       /* first sidebar entry shown (the sidebar scrolls when the window is short) */
static int setting_scroll = 0;           /* first content line shown */
static int setting_hscroll = 0;          /* content columns scrolled off to the left */
static int setting_armed = -1;           /* Power page: the line that was clicked once and now wants a second click */
static int setting_desk_idx = 0;         /* Display page: which desktop preset (0 = the theme's own) */

static void system_restart(void);        /* sys/power.h, included later */
static void system_shutdown(void);

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

/* ---- the lines of each page ------------------------------------------------------------------------------------------------
 * A page is a list of text lines built on demand (so a toggle's new value shows at once and a language switch re-words it). A line
 * is either plain text or clickable: clicking changes the thing it names. Lines may be wider than the window; the content area
 * then gets a horizontal scroll bar (and a vertical one when there are more lines than room) instead of running past the frame. */
#define SETTING_MAX_LINES 12
static const struct { const char *en, *ko; u32 bg, fg; } setting_desk[] = {
    { "Theme default", "테마 기본값", 0, 0 },                 /* 0: filled from the theme at boot (see setting_desk_apply) */
    { "Teal",   "청록",   0x008080u, 0xFFFFFFu },
    { "Gray",   "회색",   0xC0C0C0u, 0x000000u },
    { "Navy",   "남색",   0x000080u, 0xFFFFFFu },
    { "Forest", "숲",     0x2E6B3Au, 0xFFFFFFu },
    { "Plum",   "자두",   0x6B2E5Eu, 0xFFFFFFu },
};
#define SETTING_DESK_N 6
static void setting_desk_apply(void) {
    if (setting_desk_idx == 0) { desktop_bg = TH_DESKTOP; desktop_fg = TH_DESKTOP_TEXT; }
    else { desktop_bg = setting_desk[setting_desk_idx].bg; desktop_fg = setting_desk[setting_desk_idx].fg; }
}
static const char *const setting_speed_en[4] = { "Slow", "Normal", "Fast", "Faster" };
static const char *const setting_speed_ko[4] = { "느리게", "보통", "빠르게", "더 빠르게" };
static const char *const setting_dbl_en[3] = { "Slow", "Normal", "Fast" };
static const char *const setting_dbl_ko[3] = { "느리게", "보통", "빠르게" };
static const u32 setting_dbl_ticks[3] = { 90000, 60000, 36000 };
static u32 double_click_window = 60000;  /* tuned for delay(2000) per frame; adjust proportionally if delay() changes. Used by ui/input.h and MiniFiles. */

/* Builds "(*) Name" / "( ) Name" (radio, single-select -- Language) or
 * "<x> Name" / "< > Name" (checkbox, multi-select -- IME) into `out`.
 * `name` is deliberately each language's OWN name for itself
 * ("English", "한국어") rather than anything from the ui_strings
 * table -- exactly how every real language picker does it, so you can
 * still find your language even if you can't read whichever one is
 * currently active. */
static void build_option_label(char *out, u32 outsz, int is_radio, int selected, const char *name) {
    u32 len = 0;
    const char *pre = is_radio ? (selected ? "(*) " : "( ) ")
                                : (selected ? "<x> " : "< > ");
    while (*pre) kstrcpy_append(out, &len, outsz, *pre++);
    while (*name) kstrcpy_append(out, &len, outsz, *name++);
}

/* line `idx` of `page` -> out; returns 0 when the page has no such line. *click = 1 when the line does something when clicked. */
static int setting_line(int page, int idx, char *o, u32 sz, int *click) {
    u32 l = 0; o[0] = 0; *click = 1;
    switch (page) {
    case SETTING_NAV_LANGUAGE:
        if (idx == 0) { build_option_label(o, sz, 1, sys_language == LANG_ENGLISH, "English"); return 1; }
        if (idx == 1) { build_option_label(o, sz, 1, sys_language == LANG_KOREAN, "한국어"); return 1; }
        return 0;
    case SETTING_NAV_IME:
        if (idx == 0) { build_option_label(o, sz, 0, ime_enabled[IME_ENGLISH], "English"); return 1; }
        if (idx == 1) { build_option_label(o, sz, 0, ime_enabled[IME_KOREAN], "한국어"); return 1; }
        return 0;
    case SETTING_NAV_TIMEZONE:
        if (idx == 0) { build_tz_label(o, sz); *click = 0; return 1; }
        if (idx == 1) { append_str(o, &l, sz, "[-] "); append_str(o, &l, sz, SKO("Earlier hour", "한 시간 빨리")); return 1; }
        if (idx == 2) { append_str(o, &l, sz, "[+] "); append_str(o, &l, sz, SKO("Later hour", "한 시간 늦게")); return 1; }
        return 0;
    case SETTING_NAV_DISPLAY:
        if (idx == 0) { append_str(o, &l, sz, SKO("Clock: ", "시계: ")); append_str(o, &l, sz, clock_24h ? SKO("24-hour", "24시간제") : SKO("12-hour", "12시간제")); return 1; }
        if (idx == 1) { append_str(o, &l, sz, SKO("Desktop: ", "바탕화면: ")); append_str(o, &l, sz, SKO(setting_desk[setting_desk_idx].en, setting_desk[setting_desk_idx].ko)); return 1; }
        if (idx == 2) { append_str(o, &l, sz, SKO("Click a line to change it.", "줄을 누르면 바뀝니다.")); *click = 0; return 1; }
        return 0;
    case SETTING_NAV_MOUSE:
        if (idx == 0) { append_str(o, &l, sz, SKO("Pointer speed: ", "포인터 속도: ")); append_str(o, &l, sz, SKO(setting_speed_en[mouse_speed & 3], setting_speed_ko[mouse_speed & 3])); return 1; }
        if (idx == 1) {
            int d = 1; for (int i = 0; i < 3; i++) if (double_click_window == setting_dbl_ticks[i]) d = i;
            append_str(o, &l, sz, SKO("Double-click: ", "더블클릭: ")); append_str(o, &l, sz, SKO(setting_dbl_en[d], setting_dbl_ko[d])); return 1;
        }
        if (idx == 2) { append_str(o, &l, sz, SKO("Click a line to change it.", "줄을 누르면 바뀝니다.")); *click = 0; return 1; }
        return 0;
    case SETTING_NAV_SOUND:
        if (idx == 0) { append_str(o, &l, sz, SKO("Beep: ", "경고음: ")); append_str(o, &l, sz, sound_enabled ? SKO("On", "켬") : SKO("Off", "끔")); return 1; }
        if (idx == 1) { append_str(o, &l, sz, SKO("Off silences the beep", "끄면 프로그램이 요청하는")); *click = 0; return 1; }
        if (idx == 2) { append_str(o, &l, sz, SKO("that programs ask for.", "삑 소리가 나지 않습니다.")); *click = 0; return 1; }
        return 0;
    case SETTING_NAV_TEXT:
        if (idx == 0) { append_str(o, &l, sz, SKO("Kerning: ", "커닝: ")); append_str(o, &l, sz, font_kerning ? SKO("On", "켬") : SKO("Off", "끔")); return 1; }
        if (idx == 1) { append_str(o, &l, sz, SKO("Tightens pairs like AV and To", "AV, To 같은 글자 쌍의")); *click = 0; return 1; }
        if (idx == 2) { append_str(o, &l, sz, SKO("in menus, buttons and labels.", "간격을 좁힙니다 (메뉴, 버튼).")); *click = 0; return 1; }
        if (idx == 3) { append_str(o, &l, sz, SKO("MiniShell, Notepad: fixed-width.", "MiniShell, 메모장은 고정폭.")); *click = 0; return 1; }
        return 0;
    case SETTING_NAV_ABOUT: {
        *click = 0;
        if (idx == 0) { append_str(o, &l, sz, "MiniWin "); append_str(o, &l, sz, MW_VERSION); return 1; }
        if (idx == 1) { append_str(o, &l, sz, MW_SLOGAN); return 1; }
        if (idx == 2) { char cpu[64]; sysinfo_cpu_name(cpu); append_str(o, &l, sz, "CPU: "); append_str(o, &l, sz, cpu); return 1; }
        if (idx == 3) { append_str(o, &l, sz, SKO("Memory: ", "메모리: ")); append_uint(o, &l, sz, sysinfo_ram_kb() / 1024u); append_str(o, &l, sz, " MiB"); return 1; }
        if (idx == 4) { append_str(o, &l, sz, SKO("Display: ", "화면: ")); append_uint(o, &l, sz, VGA_WIDTH); append_str(o, &l, sz, "x"); append_uint(o, &l, sz, VGA_HEIGHT); return 1; }
        return 0;
    }
    case SETTING_NAV_POWER:
        if (idx == 0) { append_str(o, &l, sz, setting_armed == 0 ? SKO("Click again to restart", "다시 누르면 다시 시작") : SKO("Restart", "다시 시작")); return 1; }
        if (idx == 1) { append_str(o, &l, sz, setting_armed == 1 ? SKO("Click again to shut down", "다시 누르면 종료") : SKO("Shut down", "종료")); return 1; }
        if (idx == 2) { append_str(o, &l, sz, SKO("Click twice to confirm.", "확인하려면 두 번 누르세요.")); *click = 0; return 1; }
        return 0;
    }
    return 0;
}

/* ---- text measured in character CELLS (a Hangul syllable is one cell, like an ASCII letter); scrolling is in cells ---- */
static int setting_cells(const char *s) { int c = 0; while (*s) { s += (ko_utf8_char_len((unsigned char)*s) == 3) ? 3 : 1; c++; } return c; }
/* The text from cell `skip` on, as many characters as fit in `maxpx` PIXELS (kerned text is narrower than its cell count says, so
 * counting cells would cut a line off while there is still room for its last letters). */
static void setting_slice(const char *s, int skip, int maxpx, char *o, u32 osz) {
    u32 k = 0; int c = 0;
    while (*s && c < skip) { s += (ko_utf8_char_len((unsigned char)*s) == 3) ? 3 : 1; c++; }
    while (*s) {
        int n = (ko_utf8_char_len((unsigned char)*s) == 3) ? 3 : 1;
        if (k + n >= osz) break;
        for (int j = 0; j < n; j++) o[k + j] = s[j];
        o[k + n] = 0;
        if (ko_string_width(o) > maxpx) break;                      /* this character no longer fits */
        k += n; s += n;
    }
    o[k] = 0;
}

/* ---- geometry of the content area (and which bars it needs) ---- */
typedef struct { int x, y, w, h, n, rows, cols, maxc, maxpx, vbar, hbar; } setting_view_t;
static void setting_view(setting_view_t *v) {
    char buf[112]; int click, n = 0, maxc = 0, maxpx = 0;
    while (n < SETTING_MAX_LINES && setting_line(setting_page, n, buf, sizeof buf, &click)) {
        int c = setting_cells(buf), px = ko_string_width(buf);
        if (c > maxc) maxc = c;
        if (px > maxpx) maxpx = px;
        n++;
    }
    int fw = setting.w - SETTING_SIDEBAR_W - 8, fh = setting.h - TITLEBAR_H - 8;
    int vbar = 0, hbar = 0, rows = 1, cols = 1;
    for (int pass = 0; pass < 3; pass++) {                       /* a bar takes room, which can make the other one necessary: settle in 3 passes */
        int w = fw - (vbar ? SB_W + 2 : 0), h = fh - (hbar ? SB_W : 0);
        rows = h / SETTING_ROW_GAP; if (rows < 1) rows = 1;
        cols = w / FONT_CELL;       if (cols < 1) cols = 1;
        vbar = n > rows; hbar = maxpx > w;                         /* sideways scrolling is decided by PIXELS: kerned lines are narrower than cells*11 */
    }
    { int w = fw - (vbar ? SB_W + 2 : 0), h = fh - (hbar ? SB_W : 0);
      rows = h / SETTING_ROW_GAP; if (rows < 1) rows = 1; cols = w / FONT_CELL; if (cols < 1) cols = 1; v->w = w; v->h = h; }
    v->x = setting.x + SETTING_SIDEBAR_W + 5; v->y = setting.y + TITLEBAR_H + 5;
    v->n = n; v->rows = rows; v->cols = cols; v->maxc = maxc; v->maxpx = maxpx; v->vbar = vbar; v->hbar = hbar;
    int ms = n - rows; if (ms < 0) ms = 0; if (setting_scroll > ms) setting_scroll = ms; if (setting_scroll < 0) setting_scroll = 0;
    int mh = maxc - cols; if (mh < 0) mh = 0; if (setting_hscroll > mh) setting_hscroll = mh; if (setting_hscroll < 0) setting_hscroll = 0;
}
/* the sidebar: how many entries fit under the "SYSTEM" header, and whether it needs its own bar */
static inline int setting_nav_y0(void) { return setting_header_y() + 14; }
static int setting_nav_vis(void) { int n = (setting.h - TITLEBAR_H - 4 - 14 - 3) / SETTING_NAV_ITEM_H; return n < 1 ? 1 : n; }
static inline int setting_nav_bar(void) { return SETTING_NAV_COUNT > setting_nav_vis(); }
static inline int setting_nav_item_y(int i) { return setting_nav_y0() + (i - setting_nav_scroll) * SETTING_NAV_ITEM_H; }
static inline int setting_nav_item_w(void) { return SETTING_SIDEBAR_W - 3 - (setting_nav_bar() ? SB_W + 1 : 0); }

static void draw_setting_window(void) {
    int wx = setting.x, wy = setting.y, ww = setting.w, wh = setting.h;

    ui_window_frame_ic(wx, wy, ww, wh, setting.maximized, "SETTING.MWP", IC_SETTING);

    /* the three title-bar buttons (this window keeps its own 3px/2px inset -- see setting_btn_*()) */
    ui_titlebar_buttons(setting_btn_min_x(), setting_btn_max_x(), setting_btn_close_x(), setting_btn_y(),
                        pressed_btn_kind == BTN_MIN && pressed_btn_win == WIN_ID_SETTING,
                        pressed_btn_kind == BTN_MAX && pressed_btn_win == WIN_ID_SETTING,
                        pressed_btn_kind == BTN_CLOSE && pressed_btn_win == WIN_ID_SETTING);

    /* sidebar / content divider */
    int body_y = wy + TITLEBAR_H + 1;
    int body_h = wh - TITLEBAR_H - 2;
    ui_vline(wx + SETTING_SIDEBAR_W, body_y, body_h, TH_SHADOW);

    /* "SYSTEM" section header, then the navigable pages under it (as many as fit; the rest scroll) */
    ko_draw_mixed_string(wx + 3, body_y + 2, t(STR_SYSTEM), TH_TEXT);
    { int mx = SETTING_NAV_COUNT - setting_nav_vis(); if (mx < 0) mx = 0; if (setting_nav_scroll > mx) setting_nav_scroll = mx; }
    int vis = setting_nav_vis();
    for (int i = setting_nav_scroll; i < SETTING_NAV_COUNT && i < setting_nav_scroll + vis; i++) {
        int ny = setting_nav_item_y(i);
        u32 fg = ui_item(wx + 2, ny, setting_nav_item_w(), SETTING_NAV_ITEM_H, setting_page == i);
        ko_draw_mixed_string(wx + 6, ny + 1, mb_str(&setting_nav_txt[i]), fg);
    }
    if (setting_nav_bar()) sb_draw(SB_OWNER_SETTING_NAV);

    /* content area for whichever page is active: the visible lines, from the visible columns */
    setting_view_t v; setting_view(&v);
    char line[112], shown[112]; int click;
    for (int i = setting_scroll; i < v.n && i < setting_scroll + v.rows; i++) {
        setting_line(setting_page, i, line, sizeof line, &click);
        setting_slice(line, setting_hscroll, v.w, shown, sizeof shown);
        ko_draw_mixed_string(v.x, v.y + (i - setting_scroll) * SETTING_ROW_GAP + 1, shown, TH_TEXT);
    }
    if (v.vbar) sb_draw(WIN_ID_SETTING);
    if (v.hbar) sb_draw_h(WIN_ID_SETTING);
}

/* A click inside the window (title bar buttons are handled by the caller). Returns 0 = not mine, 1 = handled, 2 = "keep at least one
 * IME" (the caller shows that message), 3 = the language changed (the caller refreshes the status line). */
static int setting_click(int mx, int my) {
    int vis = setting_nav_vis();
    for (int i = setting_nav_scroll; i < SETTING_NAV_COUNT && i < setting_nav_scroll + vis; i++) {
        if (in_rect(mx, my, setting.x + 2, setting_nav_item_y(i), setting_nav_item_w(), SETTING_NAV_ITEM_H)) {
            if (setting_page != i) { setting_page = i; setting_scroll = 0; setting_hscroll = 0; setting_armed = -1; }
            return 1;
        }
    }
    setting_view_t v; setting_view(&v);
    if (!in_rect(mx, my, v.x, v.y, v.w, v.rows * SETTING_ROW_GAP)) return 0;
    int idx = setting_scroll + (my - v.y) / SETTING_ROW_GAP;
    char buf[112]; int click;
    if (idx >= v.n || !setting_line(setting_page, idx, buf, sizeof buf, &click) || !click) { setting_armed = -1; return 1; }

    int armed = setting_armed; setting_armed = -1;
    switch (setting_page) {
    case SETTING_NAV_LANGUAGE:
        sys_language = idx == 0 ? LANG_ENGLISH : LANG_KOREAN;
        return 3;
    case SETTING_NAV_IME: {
        int me = idx == 0 ? IME_ENGLISH : IME_KOREAN, other = idx == 0 ? IME_KOREAN : IME_ENGLISH;
        /* multi-select checkbox -- refuse to uncheck the last remaining enabled IME, same as real OSes never let you remove your only keyboard layout */
        if (ime_enabled[me] && !ime_enabled[other]) return 2;
        ime_enabled[me] = !ime_enabled[me];
        ime_ensure_current_enabled();
        return 1;
    }
    case SETTING_NAV_TIMEZONE:
        if (idx == 1 && tz_offset_hours > -12) tz_offset_hours--;
        if (idx == 2 && tz_offset_hours < 14) tz_offset_hours++;
        return 1;
    case SETTING_NAV_DISPLAY:
        if (idx == 0) clock_24h = !clock_24h;
        if (idx == 1) { setting_desk_idx = (setting_desk_idx + 1) % SETTING_DESK_N; setting_desk_apply(); }
        return 1;
    case SETTING_NAV_MOUSE:
        if (idx == 0) mouse_speed = (mouse_speed + 1) & 3;
        if (idx == 1) { int d = 1; for (int i = 0; i < 3; i++) if (double_click_window == setting_dbl_ticks[i]) d = i; double_click_window = setting_dbl_ticks[(d + 1) % 3]; }
        return 1;
    case SETTING_NAV_SOUND:
        sound_enabled = !sound_enabled;
        if (sound_enabled) { speaker_set_frequency(880); speaker_on(); delay_ms(60); speaker_off(); }     /* a short "this is what it sounds like" */
        return 1;
    case SETTING_NAV_TEXT:
        font_kerning = !font_kerning;
        return 1;
    case SETTING_NAV_POWER:
        if (armed == idx) { if (idx == 0) system_restart(); else system_shutdown(); }
        else setting_armed = idx;                                  /* the first click only arms the line */
        return 1;
    }
    return 1;
}

#endif
