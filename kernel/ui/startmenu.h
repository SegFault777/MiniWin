#ifndef MW_UI_STARTMENU_H
#define MW_UI_STARTMENU_H

/* ui/startmenu.h -- the Start menu and its power submenu.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Start Menu -- pops up above the AM button, Windows-95-style: a dark
 * vertical banner strip down the left side, a short list of items on
 * the right, and a power icon pinned to the bottom behind a divider,
 * which cascades into a small Shut Down / Restart flyout instead of
 * doing anything on its own -- because even a two-app OS deserves a
 * proper "are you sure" ceremony before it turns itself off.
 * ============================================================ */
#define STARTMENU_BANNER_W  18
#define STARTMENU_W         168   /* fits "NOTEPAD.MWP" (11 glyphs @ FONT_CELL) plus banner + margin */
#define STARTMENU_H         121
#define STARTMENU_ITEM_H    17
#define STARTMENU_ITEMS     5   /* 0=Notepad.mwp, 1=Setting.mwp, 2=Web.mwp, 3=Terminal.mwp, 4=power */
#define STARTMENU_POWER_IDX (STARTMENU_ITEMS - 1)

static inline int start_menu_x(void) { return STARTBTN_X; }
static inline int start_menu_y(void) { return TASKBAR_Y - STARTMENU_H; }

/* Notepad/Setting are grouped near the top; the power item sits glued
 * to the bottom edge behind its own divider, exactly like the real
 * thing -- no matter how few programs you have installed, it never
 * moves. */
static inline int start_menu_item_y(int idx) {
    if (idx == STARTMENU_ITEMS - 1)
        return start_menu_y() + STARTMENU_H - STARTMENU_ITEM_H - 4;
    return start_menu_y() + 6 + idx * STARTMENU_ITEM_H;
}

static int start_menu_item_hit(int px, int py, int idx) {
    int x = start_menu_x() + STARTMENU_BANNER_W + 3;
    int w = STARTMENU_W - STARTMENU_BANNER_W - 6;
    return in_rect(px, py, x, start_menu_item_y(idx), w, STARTMENU_ITEM_H);
}

/* An 11x11 hand-drawn "power" glyph -- a ring with a vertical stroke
 * poking through the gap at the top, same silhouette as the standard
 * IEC power symbol. Drawn the same way font glyphs are (row-by-row
 * bitmask), it just isn't in the text font, so it gets its own
 * function instead of a character code. Scaled up from the original
 * 8x8 version by hand (not auto-generated the way the text font is --
 * this one little glyph didn't justify its own BDF/tool pipeline) to
 * match FONT_CELL so it doesn't look like a shrunken afterthought next
 * to the 11px text sitting right beside it in the Start Menu. */
static const u16 power_icon_bits[11] = {
    0x0C00, 0x1E00, 0x1E00, 0x3300, 0x6180, 0x6180,
    0x6180, 0x3300, 0x1E00, 0x0000, 0x0000
};
static void draw_power_icon(int x, int y, u32 color) {
    for (int row = 0; row < FONT_CELL; row++) {
        u16 bits = power_icon_bits[row];
        for (int col = 0; col < FONT_CELL; col++) {
            if (bits & (0x8000 >> col)) bb_putpixel(x + col, y + row, color);
        }
    }
}

/* Whether the Shut Down / Restart flyout is showing. Only ever true
 * while start_menu_open is also true -- it's a cascade OFF the Start
 * Menu, not a thing that can exist on its own. */
static int power_menu_open = 0;

#define POWERMENU_W       116  /* fits "SHUT DOWN" (9 glyphs @ FONT_CELL) plus margin */
#define POWERMENU_ITEM_H  17
#define POWERMENU_ITEMS   2   /* 0=Shut Down, 1=Restart */
#define POWERMENU_H       (POWERMENU_ITEMS * POWERMENU_ITEM_H + 8)

/* Cascades off the right edge of the Start Menu, biased toward the
 * bottom (flush with the taskbar) rather than centered -- it's growing
 * out of the power item, which is itself pinned to the Start Menu's
 * bottom edge. */
static inline int power_menu_x(void) { return start_menu_x() + STARTMENU_W; }
static inline int power_menu_y(void) { return TASKBAR_Y - POWERMENU_H; }

static inline int power_menu_item_y(int idx) { return power_menu_y() + 3 + idx * POWERMENU_ITEM_H; }

static int power_menu_item_hit(int px, int py, int idx) {
    return in_rect(px, py, power_menu_x() + 2, power_menu_item_y(idx), POWERMENU_W - 4, POWERMENU_ITEM_H);
}

static void draw_power_menu(int mx, int my) {
    int x = power_menu_x(), y = power_menu_y(), w = POWERMENU_W, h = POWERMENU_H;
    ui_panel(x, y, w, h, TH_MENU_SHADOW);

    const char *labels[POWERMENU_ITEMS] = { t(STR_SHUT_DOWN), t(STR_RESTART) };
    for (int i = 0; i < POWERMENU_ITEMS; i++) {
        int iy = power_menu_item_y(i);
        int hover = in_rect(mx, my, x + 2, iy, w - 4, POWERMENU_ITEM_H);
        u32 fg = ui_item(x + 2, iy, w - 4, POWERMENU_ITEM_H, hover);
        ko_draw_mixed_string(x + 5, iy + 2, labels[i], fg);
    }
}

static void draw_start_menu(int mx, int my) {
    int x = start_menu_x(), y = start_menu_y();
    int w = STARTMENU_W, h = STARTMENU_H;

    /* same drop-shadow-plus-outline recipe as the File dropdown and the
     * confirm dialog -- three popups, one visual language, zero effort
     * spent reinventing it each time */
    ui_panel(x, y, w, h, TH_MENU_SHADOW);

    /* left banner: solid navy strip with the product name climbing up
     * it bottom-to-top. This is the single most 1995 thing in this
     * entire codebase and we regret nothing. */
    bb_fillrect(x + 1, y + 1, STARTMENU_BANNER_W - 1, h - 2, TH_ACCENT);
    font_draw_string_vertical(x + 3, y + h - 9, "MINIWIN", TH_ACCENT_TEXT);

    const char *labels[4] = {"Notepad.mwp", "Setting.mwp", "Web.mwp", "Terminal.mwp"};
    int ix = x + STARTMENU_BANNER_W + 2;
    int iw = w - STARTMENU_BANNER_W - 4;
    for (int i = 0; i < STARTMENU_ITEMS; i++) {
        int iy = start_menu_item_y(i);
        /* while the flyout is open, the power row stays highlighted --
         * it's the thing the cascade is coming from, same as a real
         * cascading menu keeps its parent item lit */
        int hover = (i == STARTMENU_POWER_IDX && power_menu_open)
                        || in_rect(mx, my, ix, iy, iw, STARTMENU_ITEM_H);
        u32 fg = ui_item(ix, iy, iw, STARTMENU_ITEM_H, hover);
        if (i == STARTMENU_POWER_IDX) {
            draw_power_icon(ix + 3, iy + 2, fg);
        } else {
            ko_draw_mixed_string(ix + 3, iy + 2, labels[i], fg);
        }
    }

    /* divider directly above the power item, the traditional "heads up,
     * something drastic is about to happen" line */
    int sep_y = start_menu_item_y(STARTMENU_POWER_IDX) - 2;
    ui_hline(ix, sep_y, iw, TH_SHADOW);

    if (power_menu_open) draw_power_menu(mx, my);
}

#endif
