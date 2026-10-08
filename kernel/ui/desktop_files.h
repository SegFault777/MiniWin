#ifndef MW_UI_DESKTOP_FILES_H
#define MW_UI_DESKTOP_FILES_H

/* ui/desktop_files.h -- the desktop file icons for saved documents.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Desktop file icons -- one per occupied slot (up to FS_MAX_FILES),
 * arranged in a row below the app icon since the 320x200 screen has much
 * more spare width than height. Labels are short ("DOC", "DOC2", ...)
 * rather than the full filename, since there isn't room for 4 full
 * "NEWDOC_N.TXT" labels side by side -- the real saved filename is
 * still the proper one on disk and in status messages, this is just a
 * compact on-screen label.
 * ============================================================ */
#define FILEICON_ROW_Y      (ICON_Y + ICON_H + 8)
#define FILEICON_SPACING_X  56  /* fits "DOC4" (4 glyphs @ FONT_CELL) plus margin */

static const char *fileicon_labels[FS_MAX_FILES] = {"DOC", "DOC2", "DOC3", "DOC4"};

static inline int fileicon_x(int slot) { return ICON_X + slot * FILEICON_SPACING_X; }
static inline int fileicon_y(int slot) { (void)slot; return FILEICON_ROW_Y; }

static void draw_desktop_file_icons(void) {
    for (int slot = 0; slot < FS_MAX_FILES; slot++) {
        if (!desktop_file_exists[slot]) continue;
        int x = fileicon_x(slot), y = fileicon_y(slot);
        blit_icon32(x + 6, y, ICONC_DOC);
        font_draw_string(x, y + ICON_GLYPH_H + 4, fileicon_labels[slot], TH_DESKTOP_TEXT);
    }
}

#endif
