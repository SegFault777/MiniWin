/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_UI_DESKTOP_FILES_H
#define MW_UI_DESKTOP_FILES_H

/* ui/desktop_files.h -- the desktop shortcuts for saved documents and installed programs (the cells after the app icons).
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Desktop shortcuts -- documents and programs, the cells after the five app icons in the desktop grid (icons.h): top to
 * bottom, and when the next one would not fit above the bottom edge, at the top of the column to the RIGHT (like a real desktop).
 * Documents come first (slot order), then programs (slot order). A document opens in Notepad on a double click; a program
 * runs. Labels are two lines like the app icons -- the name (up to 7 glyphs, longer ones are cut with a "~") and its
 * extension -- because the real names are user-chosen now (Notepad's Save As) and no longer the fixed "DOC".."DOC4".
 * ============================================================ */
/* (the grid itself -- DESK_CELL_X/Y, DESK_PER_COLUMN, DESK_BUILTIN_COUNT -- is in icons.h: the five app icons are its first cells.) */

#define DESK_KIND_DOC   0
#define DESK_KIND_PROG  1
/* (desktop_prog_exists[], desktop_prog_name[] and desktop_refresh_programs() live in apps/notepad/file_menu.h, next to
 * desktop_file_exists[], because MiniFiles needs them before this file is included.) */

/* The n-th shortcut (0-based, documents then programs): its kind and slot, or 0 if there is no n-th. */
static int desk_item(int n, int *kind, int *slot) {
    int k = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) if (desktop_file_exists[i]) { if (k++ == n) { *kind = DESK_KIND_DOC; *slot = i; return 1; } }
    for (int i = 0; i < PROG_MAX_SLOTS; i++) if (desktop_prog_exists[i]) { if (k++ == n) { *kind = DESK_KIND_PROG; *slot = i; return 1; } }
    return 0;
}
static inline int desk_item_x(int n) { return DESK_CELL_X(DESK_BUILTIN_COUNT + n); }
static inline int desk_item_y(int n) { return DESK_CELL_Y(DESK_BUILTIN_COUNT + n); }

/* Splits "NAME.EXT" into a label stem (<= 7 glyphs, "~" if cut) and the extension (<= 4 glyphs incl. the dot). */
static void desk_label(const char *name, char *stem, char *ext) {
    int dot = -1, n = 0;
    while (name[n]) { if (name[n] == '.') dot = n; n++; }
    int stem_len = dot > 0 ? dot : n;
    int k = 0;
    for (; k < stem_len && k < 7; k++) stem[k] = name[k];
    if (stem_len > 7) stem[6] = '~';
    stem[k] = 0;
    int e = 0;
    if (dot > 0) for (int j = dot; name[j] && e < 4; j++) ext[e++] = name[j];
    ext[e] = 0;
}

static void draw_desktop_file_icons(void) {
    int kind, slot;
    for (int n = 0; desk_item(n, &kind, &slot); n++) {
        int x = desk_item_x(n), y = desk_item_y(n);
        char stem[8], ext[5];
        desk_label(kind == DESK_KIND_DOC ? fs_doc_name(slot) : desktop_prog_name[slot], stem, ext);
        blit_icon32(x + (ICON_SLOT_W - ICON_GLYPH_W) / 2, y, kind == DESK_KIND_DOC ? ICONC_DOC : ICONC_PROG);
        int n1 = 0, n2 = 0; while (stem[n1]) n1++; while (ext[n2]) n2++;
        font_draw_string(x + (ICON_SLOT_W - FONT_CELL * n1) / 2, y + ICON_GLYPH_H + 4, stem, TH_DESKTOP_TEXT);
        if (n2) font_draw_string(x + (ICON_SLOT_W - FONT_CELL * n2) / 2, y + ICON_GLYPH_H + 4 + FONT_CELL + 1, ext, TH_DESKTOP_TEXT);
    }
}

#endif
