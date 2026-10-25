/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_UI_ICON16_H
#define MW_UI_ICON16_H

/* ui/icon16.h -- the small icons (16x16) every part of the UI shares: folder tree and file lists, window title bars, taskbar
 * pills, the Start menu, dialogs. Before alpha-10 only MiniFiles had four of them in a private cache; now there is one cache
 * (memmap.h: MW_ICON16_ADDR, 36 slots) and one way to ask for an icon.
 *
 * HOW IT WORKS: IC_* names an icon; ic16_names[] says which catalog entry (tools/install_icons.py, upper-cased file stem) it
 * is. The first time an icon is drawn it is read from the catalog (one name lookup + 2 sectors) and converted from the
 * catalog's RGBA bytes to 0xAARRGGBB; after that drawing it is a memory copy. A catalog entry that is missing (an image
 * built without the icon pack) is remembered as missing and simply draws nothing -- the caller's layout does not change,
 * there is just an empty 16x16 box.
 *
 * Two sizes come from the same bitmap: ic16_blit() draws it as is; ic16_blit12() shrinks it to 12x12 with a bilinear
 * filter, because a title bar is 15 pixels tall and a taskbar pill 14 -- a 16 pixel icon does not fit in either, and a
 * second set of 12x12 artwork would be a second thing to keep in step with the first.
 * Requires: vga.h (backbuf), fs.h (the icon catalog). Drawing never loads more than one icon per call. */

enum {
    IC_DOC, IC_PROG, IC_FOLDER, IC_FOLDER_OPEN, IC_PC, IC_DRIVE, IC_IDE, IC_SATA, IC_NVME, IC_SCSI, IC_USB, IC_FLOPPY, IC_CD,
    IC_HTML, IC_CSS, IC_JS, IC_C, IC_ASM, IC_NOTEPAD, IC_SETTING, IC_WEB, IC_TERM, IC_FILES, IC_WARNING, IC_ERROR, IC_HELP,
    IC_POWER, IC_REFRESH, IC_TRASH, IC_SEARCH, IC_SECURITY, IC_EARTH, IC_UNKNOWN, IC_NEXT, IC_UP,
    IC16_COUNT
};
typedef char ic16_assert_slots[(IC16_COUNT <= MW_ICON16_MAX) ? 1 : -1];

/* The catalog names (tools/install_icons.py upper-cases the 16x16 file stem). IC_UP is the pack's "Open2" (the green arrow
 * that leaves a folder); IC_FOLDER_OPEN is its "Open" (the folder with its lid up). */
static const char *const ic16_names[IC16_COUNT] = {
    "DOC", "PROGRAM", "FOLDER", "OPEN", "PC", "DRIVE", "IDE_DRIVE", "SATA_DRIVE", "NVME_DRIVE", "SCSI_DRIVE", "USB_2_0", "FD_144", "CD-ROM",
    "HTML_CODE", "CSS_CODE", "JAVASCRIPT_CODE", "C_CODE", "ASM_CODE", "NOTEPAD", "SETTING", "WEB", "TERMINAL", "FILEMANAGER", "WARNING", "ERROR", "HELP",
    "POWER", "REFRESH", "TRASHCAN", "SEARCH", "SECURITY", "EARTH", "UNKNOWN_DRIVE", "NEXT", "OPEN2"
};

#define ic16_cache ((u32 (*)[16 * 16])MW_ICON16_ADDR)
static u8 ic16_state[IC16_COUNT];          /* 0 = not tried yet, 1 = loaded, 2 = not in the catalog */

/* Makes sure icon `idx` is in the cache. 1 = it can be drawn, 0 = no such icon. */
static int ic16_ready(int idx) {
    if (idx < 0 || idx >= IC16_COUNT) return 0;
    if (ic16_state[idx]) return ic16_state[idx] == 1;
    ic16_state[idx] = 2;
    int slot = icon_find_by_name(ic16_names[idx]);
    u8 raw[ICON_16_BYTES];
    if (slot < 0 || !icon_load_slot(slot, 0, raw)) return 0;
    for (int k = 0; k < 16 * 16; k++) {
        u32 r = raw[k * 4], g = raw[k * 4 + 1], b = raw[k * 4 + 2], a = raw[k * 4 + 3];
        ic16_cache[idx][k] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    ic16_state[idx] = 1;
    return 1;
}

/* One source pixel over the backbuffer pixel at (px,py): straight alpha, same rules as blit_icon32() in ui/icons.h. */
static inline void ic16_plot(int px, int py, u32 rgb, u32 a) {
    if (a == 0 || px < 0 || py < 0 || px >= VGA_WIDTH || py >= VGA_HEIGHT) return;
    u32 *dst = &backbuf[py * VGA_WIDTH + px];
    if (a >= 255) { *dst = rgb & 0x00FFFFFFu; return; }
    u32 d = *dst;
    u32 r = (((rgb >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
    u32 g = (((rgb >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
    u32 b = ((rgb & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
    *dst = (r << 16) | (g << 8) | b;
}

/* The icon at its real size, top-left at (x,y). */
static void ic16_blit(int x, int y, int idx) {
    if (!ic16_ready(idx)) return;
    const u32 *src = ic16_cache[idx];
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 16; i++) { u32 p = src[j * 16 + i]; ic16_plot(x + i, y + j, p, p >> 24); }
}

/* The icon shrunk to 12x12 (bilinear on alpha-weighted colour, so a transparent edge does not turn into a dark fringe).
 * Destination pixel i samples source coordinate (2i+1)*16/24 - 1/2, kept in 8.8 fixed point; the four neighbours get weights
 * that add up to ~256 (w>>8), each scaled by that pixel's alpha. Everything stays below 2^32: 256 x 255 x 255 = 16.6M. */
static void ic16_blit12(int x, int y, int idx) {
    if (!ic16_ready(idx)) return;
    const u32 *src = ic16_cache[idx];
    for (int j = 0; j < 12; j++) {
        int sy = (int)(((2 * j + 1) * 2048) / 12) - 128; if (sy < 0) sy = 0;
        int y0 = sy >> 8, fy = sy & 255, y1 = y0 + 1 > 15 ? 15 : y0 + 1;
        for (int i = 0; i < 12; i++) {
            int sx = (int)(((2 * i + 1) * 2048) / 12) - 128; if (sx < 0) sx = 0;
            int x0 = sx >> 8, fx = sx & 255, x1 = x0 + 1 > 15 ? 15 : x0 + 1;
            u32 w[4] = { (u32)((256 - fx) * (256 - fy)) >> 8, (u32)(fx * (256 - fy)) >> 8, (u32)((256 - fx) * fy) >> 8, (u32)(fx * fy) >> 8 };
            u32 p[4] = { src[y0 * 16 + x0], src[y0 * 16 + x1], src[y1 * 16 + x0], src[y1 * 16 + x1] };
            u32 wsum = 0, den = 0, sr = 0, sg = 0, sb = 0;
            for (int k = 0; k < 4; k++) {
                u32 wa = w[k] * (p[k] >> 24);                  /* weight x alpha */
                wsum += w[k]; den += wa;
                sr += wa * ((p[k] >> 16) & 0xFF); sg += wa * ((p[k] >> 8) & 0xFF); sb += wa * (p[k] & 0xFF);
            }
            if (den == 0 || wsum == 0) continue;               /* fully transparent here */
            u32 a = den / wsum;                                /* 0..255 */
            u32 rr = sr / den, gg = sg / den, bb = sb / den;
            ic16_plot(x + i, y + j, (rr << 16) | (gg << 8) | bb, a);
        }
    }
}

#endif
