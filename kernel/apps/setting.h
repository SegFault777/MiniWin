/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_APPS_SETTING_H
#define MW_APPS_SETTING_H

/* apps/setting.h -- SETTING.MWP, laid out like the Windows 95 Control Panel (1.1-alpha-13): a grid of big icons with their names under
 * them, one per kind of setting; double-click one (or select it and press Enter) and its page opens in the same window, with a Back
 * button. The page lines are the same as in alpha-12: language / keyboard (IME) / date and time / display / mouse / sounds / text / system / power.
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
#define SETTING_ROW_GAP     18
#define SETTING_MAX_LINES   12
#define CP_CELL_W           100  /* one icon cell: the 32x32 icon with its name under it (two lines at most) */
#define CP_CELL_H           74
#define CP_STATUS_H         15

/* The control-panel items, in the order Windows 95 sorts them (by name). Each opens the page of the same number. */
enum { SP_DATETIME, SP_DISPLAY, SP_KEYBOARD, SP_MOUSE, SP_POWER, SP_REGIONAL, SP_SOUND, SP_SYSTEM, SP_TEXT, SP_COUNT };
typedef struct { mb_txt_t name; mb_txt_t desc; const char *icon; } cp_item_t;
/* `icon` is the 32x32 catalog name (tools/install_icons.py, upper-cased file stem). 1.1-alpha-13: DATE_TIME, SOUND and FONT are the
 * project's own artwork (they replaced the question-mark stand-in); the catalog has 64 slots (ICON_MAX_SLOTS in fs.h). */
static const cp_item_t cp_items[SP_COUNT] = {
    { { "Date/Time", "날짜/시간" }, { "Changes the time zone and the clock.", "시간대와 시계를 바꿉니다." }, "DATE_TIME" },
    { { "Display", "화면" }, { "Changes the colour of the desktop.", "바탕화면의 색을 바꿉니다." }, "PERSONALIZATION_OR_DISPLAY" },
    { { "Keyboard", "키보드" }, { "Changes settings for your keyboard (input methods).", "키보드(입력기) 설정을 바꿉니다." }, "KEYBOARD" },
    { { "Mouse", "마우스" }, { "Changes settings for your mouse.", "마우스 설정을 바꿉니다." }, "MOUSE" },
    { { "Power", "전원" }, { "Restarts or shuts down the computer.", "컴퓨터를 다시 시작하거나 끕니다." }, "POWER" },
    { { "Regional Settings", "지역 설정" }, { "Changes the language of menus and messages.", "메뉴와 메시지의 언어를 바꿉니다." }, "EARTH" },
    { { "Sounds", "소리" }, { "Changes the sounds your computer makes.", "컴퓨터가 내는 소리를 바꿉니다." }, "SOUND" },
    { { "System", "시스템" }, { "Shows information about MiniWin and your computer.", "MiniWin과 컴퓨터의 정보를 보여 줍니다." }, "PC" },
    { { "Font", "글꼴" }, { "Changes how letters are spaced (kerning).", "글자 사이 간격(커닝)을 바꿉니다." }, "FONT" },
};
#define SKO(en, ko) (sys_language == LANG_KOREAN ? (ko) : (en))

static int setting_page = -1;            /* -1: the icon grid (the Control Panel itself); otherwise the SP_* page that is open */
static int setting_sel = 0;              /* the selected icon */
static int setting_gscroll = 0;          /* grid: first visible row of icons */
static int setting_last_sel = -1;        /* double-click bookkeeping: the icon clicked last, and when */
static u32 setting_last_tick = 0;
static int setting_scroll = 0;           /* page: first line shown */
static int setting_hscroll = 0;          /* page: columns scrolled off to the left */
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

/* ---- the 32x32 icons of the items: loaded from the icon catalog the first time the window is drawn (9 x 4 KB from the heap) ---- */
static u32 *cp_icon = 0;
static void cp_icons_load(void) {
    if (cp_icon) return;
    cp_icon = (u32 *)kzalloc((u32)SP_COUNT * 32 * 32 * 4);
    if (!cp_icon) return;
    for (int i = 0; i < SP_COUNT; i++) {
        int slot = icon_find_by_name(cp_items[i].icon);
        if (slot < 0) slot = icon_find_by_name("HELP");                  /* an icon that is not installed: the question mark */
        u8 *raw = (u8 *)(cp_icon + i * 1024);                            /* the catalog's R,G,B,A bytes land in the icon's own 4 KB ... */
        if (slot < 0 || !icon_load_slot(slot, 1, raw)) continue;
        for (int k = 0; k < 32 * 32; k++) {                              /* ... and are turned into 0xAARRGGBB words in place (pixel k reads the 4 bytes it then replaces) */
            u32 r = raw[k * 4 + 0], g = raw[k * 4 + 1], b = raw[k * 4 + 2], a = raw[k * 4 + 3];
            cp_icon[i * 1024 + k] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

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

/* line `idx` of the page `page` -> o; returns 0 when the page has no such line. *click = 1 when the line does something when clicked.
 * A line is built on demand, so a toggle's new value shows at once and a language switch re-words it. */
static int setting_line(int page, int idx, char *o, u32 sz, int *click) {
    u32 l = 0; o[0] = 0; *click = 1;
    switch (page) {
    case SP_REGIONAL:
        if (idx == 0) { build_option_label(o, sz, 1, sys_language == LANG_ENGLISH, "English"); return 1; }
        if (idx == 1) { build_option_label(o, sz, 1, sys_language == LANG_KOREAN, "한국어"); return 1; }
        return 0;
    case SP_KEYBOARD:
        if (idx == 0) { build_option_label(o, sz, 0, ime_enabled[IME_ENGLISH], "English"); return 1; }
        if (idx == 1) { build_option_label(o, sz, 0, ime_enabled[IME_KOREAN], "한국어"); return 1; }
        if (idx == 2) { append_str(o, &l, sz, SKO("Right Alt switches between the checked ones.", "오른쪽 Alt로 선택한 입력기를 바꿉니다.")); *click = 0; return 1; }
        return 0;
    case SP_DATETIME:
        if (idx == 0) { build_tz_label(o, sz); *click = 0; return 1; }
        if (idx == 1) { append_str(o, &l, sz, "[-] "); append_str(o, &l, sz, SKO("Earlier hour", "한 시간 빨리")); return 1; }
        if (idx == 2) { append_str(o, &l, sz, "[+] "); append_str(o, &l, sz, SKO("Later hour", "한 시간 늦게")); return 1; }
        if (idx == 3) { append_str(o, &l, sz, SKO("Clock: ", "시계: ")); append_str(o, &l, sz, clock_24h ? SKO("24-hour", "24시간제") : SKO("12-hour", "12시간제")); return 1; }
        return 0;
    case SP_DISPLAY:
        if (idx == 0) { append_str(o, &l, sz, SKO("Desktop: ", "바탕화면: ")); append_str(o, &l, sz, SKO(setting_desk[setting_desk_idx].en, setting_desk[setting_desk_idx].ko)); return 1; }
        if (idx == 1) { append_str(o, &l, sz, SKO("Click a line to change it.", "줄을 누르면 바뀝니다.")); *click = 0; return 1; }
        return 0;
    case SP_MOUSE:
        if (idx == 0) { append_str(o, &l, sz, SKO("Pointer speed: ", "포인터 속도: ")); append_str(o, &l, sz, SKO(setting_speed_en[mouse_speed & 3], setting_speed_ko[mouse_speed & 3])); return 1; }
        if (idx == 1) {
            int d = 1; for (int i = 0; i < 3; i++) if (double_click_window == setting_dbl_ticks[i]) d = i;
            append_str(o, &l, sz, SKO("Double-click: ", "더블클릭: ")); append_str(o, &l, sz, SKO(setting_dbl_en[d], setting_dbl_ko[d])); return 1;
        }
        if (idx == 2) { append_str(o, &l, sz, SKO("Click a line to change it.", "줄을 누르면 바뀝니다.")); *click = 0; return 1; }
        return 0;
    case SP_SOUND:
        if (idx == 0) { append_str(o, &l, sz, SKO("Beep: ", "경고음: ")); append_str(o, &l, sz, sound_enabled ? SKO("On", "켬") : SKO("Off", "끔")); return 1; }
        if (idx == 1) { append_str(o, &l, sz, SKO("Off silences the beep", "끄면 프로그램이 요청하는")); *click = 0; return 1; }
        if (idx == 2) { append_str(o, &l, sz, SKO("that programs ask for.", "삑 소리가 나지 않습니다.")); *click = 0; return 1; }
        return 0;
    case SP_TEXT:
        if (idx == 0) { append_str(o, &l, sz, SKO("Kerning: ", "커닝: ")); append_str(o, &l, sz, font_kerning ? SKO("On", "켬") : SKO("Off", "끔")); return 1; }
        if (idx == 1) { append_str(o, &l, sz, SKO("Tightens pairs like AV and To", "AV, To 같은 글자 쌍의")); *click = 0; return 1; }
        if (idx == 2) { append_str(o, &l, sz, SKO("in menus, buttons and labels.", "간격을 좁힙니다 (메뉴, 버튼).")); *click = 0; return 1; }
        if (idx == 3) { append_str(o, &l, sz, SKO("MiniShell, Notepad: fixed-width.", "MiniShell, 메모장은 고정폭.")); *click = 0; return 1; }
        return 0;
    case SP_SYSTEM: {
        *click = 0;
        if (idx == 0) { append_str(o, &l, sz, "MiniWin "); append_str(o, &l, sz, MW_VERSION); return 1; }
        if (idx == 1) { append_str(o, &l, sz, MW_SLOGAN); return 1; }
        if (idx == 2) { char cpu[64]; sysinfo_cpu_name(cpu); append_str(o, &l, sz, "CPU: "); append_str(o, &l, sz, cpu); return 1; }
        if (idx == 3) { append_str(o, &l, sz, SKO("Memory: ", "메모리: ")); append_uint(o, &l, sz, sysinfo_ram_kb() / 1024u); append_str(o, &l, sz, " MiB"); return 1; }
        if (idx == 4) { append_str(o, &l, sz, SKO("Display: ", "화면: ")); append_uint(o, &l, sz, VGA_WIDTH); append_str(o, &l, sz, "x"); append_uint(o, &l, sz, VGA_HEIGHT); return 1; }
        return 0;
    }
    case SP_POWER:
        if (idx == 0) { append_str(o, &l, sz, setting_armed == 0 ? SKO("Click again to restart", "다시 누르면 다시 시작") : SKO("Restart", "다시 시작")); return 1; }
        if (idx == 1) { append_str(o, &l, sz, setting_armed == 1 ? SKO("Click again to shut down", "다시 누르면 종료") : SKO("Shut down", "종료")); return 1; }
        if (idx == 2) { append_str(o, &l, sz, SKO("Click twice to confirm.", "확인하려면 두 번 누르세요.")); *click = 0; return 1; }
        return 0;
    }
    return 0;
}

/* the small icon in front of a line (Power: Restart and Shut down), or -1 */
static int setting_line_icon(int page, int idx) {
    if (page == SP_POWER) return idx == 0 ? IC_RESTART : (idx == 1 ? IC_POWER : -1);
    return -1;
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

/* ================================================================ geometry
 * The window, top to bottom: title bar, menu bar, the body (the icon grid, or an open page), the status bar. */
static inline int setting_body_y(void)   { return setting.y + TITLEBAR_H + MB_H + 1; }
static inline int setting_status_y(void) { return setting.y + setting.h - 3 - CP_STATUS_H; }

/* the icon grid: a sunken white field with the icons in rows, as many to a row as fit (they re-flow when the window is resized) */
typedef struct { int fx, fy, fw, fh, cols, rows, vis, vbar; } cp_grid_t;
static void cp_grid(cp_grid_t *g) {
    g->fx = setting.x + 3; g->fy = setting_body_y() + 1;
    g->fw = setting.w - 6; g->fh = setting_status_y() - 2 - g->fy;
    int vbar = 0, cols = 1;
    for (int pass = 0; pass < 2; pass++) {                       /* a scroll bar takes room, which can change the column count: settle in 2 passes */
        int w = g->fw - 4 - (vbar ? SB_W : 0);
        cols = w / CP_CELL_W; if (cols < 1) cols = 1;
        int rows = (SP_COUNT + cols - 1) / cols, vis = (g->fh - 4) / CP_CELL_H; if (vis < 1) vis = 1;
        vbar = rows > vis;
    }
    g->cols = cols; g->rows = (SP_COUNT + cols - 1) / cols; g->vis = (g->fh - 4) / CP_CELL_H; if (g->vis < 1) g->vis = 1; g->vbar = vbar;
    int ms = g->rows - g->vis; if (ms < 0) ms = 0; if (setting_gscroll > ms) setting_gscroll = ms; if (setting_gscroll < 0) setting_gscroll = 0;
}
static inline int cp_cell_x(const cp_grid_t *g, int i) { return g->fx + 2 + (i % g->cols) * CP_CELL_W; }
static inline int cp_cell_y(const cp_grid_t *g, int i) { return g->fy + 2 + (i / g->cols - setting_gscroll) * CP_CELL_H; }
static int cp_cell_at(const cp_grid_t *g, int mx, int my) {
    if (!in_rect(mx, my, g->fx + 2, g->fy + 2, g->cols * CP_CELL_W, g->vis * CP_CELL_H)) return -1;
    int i = ((my - g->fy - 2) / CP_CELL_H + setting_gscroll) * g->cols + (mx - g->fx - 2) / CP_CELL_W;
    return i < SP_COUNT ? i : -1;
}
static void cp_ensure_visible(const cp_grid_t *g) {
    int row = setting_sel / g->cols;
    if (row < setting_gscroll) setting_gscroll = row;
    if (row >= setting_gscroll + g->vis) setting_gscroll = row - g->vis + 1;
}

/* an open page: a header (Back button, the item's icon and name) over the lines; the lines scroll in both directions when they must */
#define CP_HDR_H 62
typedef struct { int x, y, w, h, n, rows, cols, maxc, maxpx, vbar, hbar; } setting_view_t;
static inline int cp_back_x(void) { return setting.x + 8; }
static inline int cp_back_y(void) { return setting_body_y() + 6; }
#define CP_BACK_W 72
#define CP_BACK_H 18
static void setting_view(setting_view_t *v) {
    char buf[112]; int click, n = 0, maxc = 0, maxpx = 0;
    while (n < SETTING_MAX_LINES && setting_line(setting_page, n, buf, sizeof buf, &click)) {
        int c = setting_cells(buf), px = ko_string_width(buf);
        if (c > maxc) maxc = c;
        if (px > maxpx) maxpx = px;
        n++;
    }
    int fw = setting.w - 22, fh = setting_status_y() - 6 - (setting_body_y() + CP_HDR_H);
    int vbar = 0, hbar = 0, rows = 1, cols = 1;
    for (int pass = 0; pass < 3; pass++) {                       /* a bar takes room, which can make the other one necessary: settle in 3 passes */
        int w = fw - (vbar ? SB_W + 2 : 0), h = fh - (hbar ? SB_W : 0);
        rows = h / SETTING_ROW_GAP; if (rows < 1) rows = 1;
        cols = w / FONT_CELL;       if (cols < 1) cols = 1;
        vbar = n > rows; hbar = maxpx > w;                         /* sideways scrolling is decided by PIXELS: kerned lines are narrower than cells*11 */
    }
    { int w = fw - (vbar ? SB_W + 2 : 0), h = fh - (hbar ? SB_W : 0);
      rows = h / SETTING_ROW_GAP; if (rows < 1) rows = 1; cols = w / FONT_CELL; if (cols < 1) cols = 1; v->w = w; v->h = h; }
    v->x = setting.x + 11; v->y = setting_body_y() + CP_HDR_H;
    v->n = n; v->rows = rows; v->cols = cols; v->maxc = maxc; v->maxpx = maxpx; v->vbar = vbar; v->hbar = hbar;
    int ms = n - rows; if (ms < 0) ms = 0; if (setting_scroll > ms) setting_scroll = ms; if (setting_scroll < 0) setting_scroll = 0;
    int mh = maxc - cols; if (mh < 0) mh = 0; if (setting_hscroll > mh) setting_hscroll = mh; if (setting_hscroll < 0) setting_hscroll = 0;
}

/* ================================================================ the menu bar */
#define SETTING_MENU_N 3
static const mb_menu_t setting_menus[SETTING_MENU_N] = {
    { { "File", "파일" }, 2, { { "Open", "열기" }, { "Close", "닫기" } } },
    { { "View", "보기" }, 1, { { "Control Panel", "제어판" } } },
    { { "Help", "도움말" }, 1, { { "About MiniWin", "MiniWin 정보" } } },
};
static void setting_open_item(int i) { if (i < 0 || i >= SP_COUNT) return; setting_page = i; setting_scroll = 0; setting_hscroll = 0; setting_armed = -1; }
static void setting_menu_action(int menu, int item) {
    if (menu == 0) { if (item == 0) { if (setting_page < 0) setting_open_item(setting_sel); } else setting.open = 0; }
    else if (menu == 1) { setting_page = -1; setting_armed = -1; }
    else { setting_sel = SP_SYSTEM; setting_open_item(SP_SYSTEM); }
}

/* ================================================================ drawing */
/* a name under an icon: two lines at most, wrapped at spaces, each centred in the cell; the selected one is a blue bar with light text */
static void cp_draw_label(int cx, int ly, const char *name, int selected) {
    char line[2][40]; int nl = 0, len = 0;
    line[0][0] = line[1][0] = 0;
    const char *p = name;
    while (*p && nl < 2) {
        const char *w = p; int wl = 0;
        while (w[wl] && w[wl] != ' ') wl++;                       /* the next word */
        char trial[40]; int tl = 0;
        for (int i = 0; i < len; i++) trial[tl++] = line[nl][i];
        if (len) trial[tl++] = ' ';
        for (int i = 0; i < wl && tl < 38; i++) trial[tl++] = w[i];
        trial[tl] = 0;
        if (len && ko_string_width(trial) > CP_CELL_W - 6) { nl++; len = 0; continue; }   /* does not fit: the word starts the next line */
        for (int i = 0; i <= tl; i++) line[nl][i] = trial[i];
        len = tl; p = w + wl; if (*p == ' ') p++;
    }
    for (int i = 0; i < 2 && line[i][0]; i++) {
        int tw = ko_string_width(line[i]), x = cx + (CP_CELL_W - tw) / 2, y = ly + i * 12;
        if (selected) bb_fillrect(x - 2, y - 1, tw + 4, 12, TH_ACCENT);
        ko_draw_mixed_string(x, y, line[i], selected ? TH_ACCENT_TEXT : TH_FIELD_TEXT);
    }
}

static void draw_setting_window(void) {
    int wx = setting.x, wy = setting.y, ww = setting.w, wh = setting.h;
    cp_icons_load();

    ui_window_frame_ic(wx, wy, ww, wh, setting.maximized, "SETTING.MWP", IC_SETTING);
    ui_titlebar_buttons(setting_btn_min_x(), setting_btn_max_x(), setting_btn_close_x(), setting_btn_y(),
                        pressed_btn_kind == BTN_MIN && pressed_btn_win == WIN_ID_SETTING,
                        pressed_btn_kind == BTN_MAX && pressed_btn_win == WIN_ID_SETTING,
                        pressed_btn_kind == BTN_CLOSE && pressed_btn_win == WIN_ID_SETTING);
    mb_draw_bar(WIN_ID_SETTING, wx, wy, ww, setting_menus, SETTING_MENU_N);

    const char *status = "";
    if (setting_page < 0) {
        /* ---- the Control Panel: the icons ---- */
        cp_grid_t g; cp_grid(&g);
        ui_field(g.fx, g.fy, g.fw, g.fh, TH_FIELD, 0);
        for (int i = 0; i < SP_COUNT; i++) {
            int row = i / g.cols;
            if (row < setting_gscroll || row >= setting_gscroll + g.vis) continue;
            int cx = cp_cell_x(&g, i), cy = cp_cell_y(&g, i);
            if (cp_icon) blit_rgba32(cx + (CP_CELL_W - 32) / 2, cy + 3, cp_icon + i * 1024);
            cp_draw_label(cx, cy + 3 + 32 + 6, mb_str(&cp_items[i].name), i == setting_sel);
        }
        if (g.vbar) sb_draw(WIN_ID_SETTING);
        status = mb_str(&cp_items[setting_sel].desc);
    } else {
        /* ---- an open page: Back, the icon and name, then its lines ---- */
        int bx = cp_back_x(), by = cp_back_y();
        ui_button(bx, by, CP_BACK_W, CP_BACK_H, "", 0, 0, 0);
        { const char *bt = SKO("Back", "뒤로"); int tw = ko_string_width(bt), ix = bx + (CP_BACK_W - (16 + 4 + tw)) / 2;
          ic16_blit(ix, by + 1, IC_PREV);                                           /* the green arrow, then the word */
          ko_draw_mixed_string(ix + 16 + 4, by + 3, bt, TH_TEXT); }
        if (cp_icon) blit_rgba32(wx + 8, by + CP_BACK_H + 6, cp_icon + setting_page * 1024);
        ko_draw_mixed_string(wx + 8 + 32 + 8, by + CP_BACK_H + 6 + 10, mb_str(&cp_items[setting_page].name), TH_TEXT);
        ui_hline(wx + 6, setting_body_y() + CP_HDR_H - 4, ww - 12, TH_SHADOW);
        ui_hline(wx + 6, setting_body_y() + CP_HDR_H - 3, ww - 12, TH_LIGHT);

        setting_view_t v; setting_view(&v);
        char line[112], shown[112]; int click;
        for (int i = setting_scroll; i < v.n && i < setting_scroll + v.rows; i++) {
            setting_line(setting_page, i, line, sizeof line, &click);
            int lic = setting_line_icon(setting_page, i), ind = (lic >= 0 || setting_page == SP_POWER) ? 20 : 0;   /* a page with icons keeps all its lines in one column */
            setting_slice(line, setting_hscroll, v.w - ind, shown, sizeof shown);
            if (lic >= 0) ic16_blit(v.x, v.y + (i - setting_scroll) * SETTING_ROW_GAP + 1, lic);
            ko_draw_mixed_string(v.x + ind, v.y + (i - setting_scroll) * SETTING_ROW_GAP + 3, shown, TH_TEXT);
        }
        if (v.vbar) sb_draw(WIN_ID_SETTING);
        if (v.hbar) sb_draw_h(WIN_ID_SETTING);
        status = mb_str(&cp_items[setting_page].desc);
    }
    /* the status bar: what the selected icon does */
    ui_readout(wx + 3, setting_status_y(), ww - 6, CP_STATUS_H, 0);
    { char clipped[96]; setting_slice(status, 0, ww - 6 - 8, clipped, sizeof clipped);          /* never wider than the bar */
      ko_draw_mixed_string(wx + 7, setting_status_y() + 2, clipped, TH_TEXT); }
    mb_draw_drop(WIN_ID_SETTING, wx, wy, setting_menus, SETTING_MENU_N);
}

/* ================================================================ input */
/* A click inside the window (the title bar buttons and the menu bar are handled by the caller). Returns 0 = not mine, 1 = handled,
 * 2 = "keep at least one IME" (the caller shows that message), 3 = the language changed (the caller refreshes the status line).
 * `now` is the tick counter, for telling a double click from two single ones. */
static int setting_click(int mx, int my, u32 now) {
    if (setting_page < 0) {
        cp_grid_t g; cp_grid(&g);
        int i = cp_cell_at(&g, mx, my);
        if (i < 0) { if (in_rect(mx, my, g.fx, g.fy, g.fw, g.fh)) return 1; return 0; }
        if (i == setting_last_sel && (now - setting_last_tick) < double_click_window) { setting_sel = i; setting_last_sel = -1; setting_open_item(i); return 1; }
        setting_sel = i; setting_last_sel = i; setting_last_tick = now;
        return 1;
    }
    if (in_rect(mx, my, cp_back_x(), cp_back_y(), CP_BACK_W, CP_BACK_H)) { setting_page = -1; setting_armed = -1; return 1; }
    setting_view_t v; setting_view(&v);
    if (!in_rect(mx, my, v.x, v.y, v.w, v.rows * SETTING_ROW_GAP)) return 0;
    int idx = setting_scroll + (my - v.y) / SETTING_ROW_GAP;
    char buf[112]; int click;
    if (idx >= v.n || !setting_line(setting_page, idx, buf, sizeof buf, &click) || !click) { setting_armed = -1; return 1; }

    int armed = setting_armed; setting_armed = -1;
    switch (setting_page) {
    case SP_REGIONAL:
        sys_language = idx == 0 ? LANG_ENGLISH : LANG_KOREAN;
        return 3;
    case SP_KEYBOARD: {
        int me = idx == 0 ? IME_ENGLISH : IME_KOREAN, other = idx == 0 ? IME_KOREAN : IME_ENGLISH;
        /* multi-select checkbox -- refuse to uncheck the last remaining enabled IME, same as real OSes never let you remove your only keyboard layout */
        if (ime_enabled[me] && !ime_enabled[other]) return 2;
        ime_enabled[me] = !ime_enabled[me];
        ime_ensure_current_enabled();
        return 1;
    }
    case SP_DATETIME:
        if (idx == 1 && tz_offset_hours > -12) tz_offset_hours--;
        if (idx == 2 && tz_offset_hours < 14) tz_offset_hours++;
        if (idx == 3) clock_24h = !clock_24h;
        return 1;
    case SP_DISPLAY:
        if (idx == 0) { setting_desk_idx = (setting_desk_idx + 1) % SETTING_DESK_N; setting_desk_apply(); }
        return 1;
    case SP_MOUSE:
        if (idx == 0) mouse_speed = (mouse_speed + 1) & 3;
        if (idx == 1) { int d = 1; for (int i = 0; i < 3; i++) if (double_click_window == setting_dbl_ticks[i]) d = i; double_click_window = setting_dbl_ticks[(d + 1) % 3]; }
        return 1;
    case SP_SOUND:
        sound_enabled = !sound_enabled;
        if (sound_enabled) { speaker_set_frequency(880); speaker_on(); delay_ms(60); speaker_off(); }     /* a short "this is what it sounds like" */
        return 1;
    case SP_TEXT:
        font_kerning = !font_kerning;
        return 1;
    case SP_POWER:
        if (armed == idx) { if (idx == 0) system_restart(); else system_shutdown(); }
        else setting_armed = idx;                                  /* the first click only arms the line */
        return 1;
    }
    return 1;
}

/* the keyboard (the window has the focus): arrows move the selection, Enter opens it; Esc / Backspace / Left in a page go back */
static void setting_key(int k) {
    if (setting_page >= 0) { if (k == 27 || k == '\b' || k == KEY_LEFT) { setting_page = -1; setting_armed = -1; } return; }
    cp_grid_t g; cp_grid(&g);
    if (k == KEY_LEFT && setting_sel > 0) setting_sel--;
    else if (k == KEY_RIGHT && setting_sel < SP_COUNT - 1) setting_sel++;
    else if (k == KEY_UP && setting_sel - g.cols >= 0) setting_sel -= g.cols;
    else if (k == KEY_DOWN && setting_sel + g.cols < SP_COUNT) setting_sel += g.cols;
    else if (k == '\n') { setting_open_item(setting_sel); return; }
    cp_ensure_visible(&g);
}

#endif
