/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_UI_ICONS_H
#define MW_UI_ICONS_H

/* ui/icons.h -- the desktop icon grid, the 32x32 icon cache, and the five app launcher icons.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Desktop icon
 * ============================================================ */
/* Both desktop icons sit in equal-width "slots" -- the icon glyph and
 * each line of its label are centered within the slot independently,
 * so a wide label (like "NOTEPAD", wider than the 22px icon above it)
 * overflows the same amount on both sides instead of trailing off to
 * one side the way raw left-aligned coordinates used to.
 *
 * ICON_SLOT_W is sized off the widest label line this UI actually
 * draws in a slot: "NOTEPAD" and "SETTING" are both 7 characters, and
 * at FONT_CELL=11 that's 77px. 88px gives that a comfortable ~11px of
 * breathing room on each side without wasting desktop space -- this
 * used to be a tighter 60px back when the font was 8px wide (7*8=56,
 * so the old slot barely fit its own label either; it never had real
 * headroom, it just happened to be small enough that nobody noticed). */
#define ICON_SLOT_W 88
#define ICON_GLYPH_W 32   /* real 32x32 bitmap icons now (see icon cache below) */
#define ICON_GLYPH_H 32

#define ICON_X   6
#define ICON_Y   8
#define ICON_W   ICON_SLOT_W

/* The desktop grid. EVERY desktop icon -- the five built-in apps, then documents, then program shortcuts (desktop_files.h) --
 * sits in one vertical list at a constant pitch (ICON_H): top to bottom from ICON_Y, and when the next cell would end below
 * DESK_BOTTOM_Y it starts at the top of the column to the RIGHT. Cell n (0-based) is at (DESK_CELL_X(n), DESK_CELL_Y(n)).
 * The columns are 10px wider than an icon slot, which is what the old horizontal row used: "MINIFILES" is a glyph wider than
 * the slot and spills into that gap. */
#define DESK_BOTTOM_Y     (VGA_HEIGHT - 48)                    /* a cell must end above this (status line + taskbar) */
#define DESK_COL_W        (ICON_SLOT_W + 10)
#define DESK_PER_COLUMN   ((DESK_BOTTOM_Y - ICON_Y) / ICON_H)
#define DESK_CELL_X(n)    (ICON_X + ((n) / DESK_PER_COLUMN) * DESK_COL_W)
#define DESK_CELL_Y(n)    (ICON_Y + ((n) % DESK_PER_COLUMN) * ICON_H)
#define DESK_BUILTIN_COUNT 5                                   /* NOTEPAD, SETTING, WEB, TERMINAL, MINIFILES */
#define ICON_H   (ICON_GLYPH_H + 4 + 11 + 1 + 11)   /* glyph + gap + two label lines */

/* ------------------------------------------------------------
 * Icon cache: the 32x32 bitmap icons installed on disk by
 * tools/install_icons.py (kernel/fs.h's icon catalog), decoded once at
 * boot into 0xAARRGGBB words so drawing one is a plain memory blit
 * instead of eight ATA sector reads per frame. The on-disk bytes are
 * R,G,B,A; the backbuffer wants 0x00RRGGBB, so the decode step is the
 * one place that byte-order mismatch fs.h warned about gets settled.
 * A missing icon (say, an os-image.img built without running
 * install_icons.py) just draws a gray placeholder box instead of
 * crashing -- the desktop stays usable, merely uglier.
 * ------------------------------------------------------------ */
#define ICONC_NOTEPAD 0
#define ICONC_SETTING 1
#define ICONC_WEB     2
#define ICONC_DOC     3
#define ICONC_TERM    4
#define ICONC_FILES   5
#define ICONC_PROG    6          /* the program shortcut glyph (the pack's "Program") */
#define ICONC_COUNT   7

static const char *const icon_cache_names[ICONC_COUNT] = { "NOTEPAD", "SETTING", "WEB", "DOC", "TERMINAL", "FILEMANAGER", "PROGRAM" };
/* 28KB: lives in memmap.h's MISC area, not .bss (always filled by icon_cache_init() before it is read) */
#define icon_cache ((u32 (*)[32 * 32])MW_ICON_CACHE_ADDR)
static u8  icon_cache_ok[ICONC_COUNT];

static void icon_cache_init(void) {
    static u8 raw[ICON_32_BYTES];
    for (int i = 0; i < ICONC_COUNT; i++) {
        icon_cache_ok[i] = 0;
        int slot = icon_find_by_name(icon_cache_names[i]);
        if (slot < 0) continue;
        if (!icon_load_slot(slot, 1, raw)) continue;
        for (int k = 0; k < 32 * 32; k++) {
            u32 r = raw[k * 4 + 0], g = raw[k * 4 + 1], b = raw[k * 4 + 2], a = raw[k * 4 + 3];
            icon_cache[i][k] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        icon_cache_ok[i] = 1;
    }
}

/* Alpha-blends one cached icon onto the backbuffer at (x,y), clipped to
 * the screen. Alpha 0 skips the pixel, 255 overwrites it, anything in
 * between mixes with whatever's already there (the icons have soft
 * antialiased edges, so this matters). */
/* A 32x32 icon from any 0xAARRGGBB buffer (a program's own icon, mwp_meta.h) over whatever is behind it. */
static void blit_rgba32(int x, int y, const u32 *src) {
    for (int j = 0; j < 32; j++) {
        int py = y + j;
        if (py < 0 || py >= VGA_HEIGHT) continue;
        for (int i = 0; i < 32; i++) {
            int px = x + i;
            if (px < 0 || px >= VGA_WIDTH) continue;
            u32 p = src[j * 32 + i];
            u32 a = p >> 24;
            if (a == 0) continue;
            u32 *dst = &backbuf[py * VGA_WIDTH + px];
            if (a == 255) { *dst = p & 0x00FFFFFFu; continue; }
            u32 d = *dst;
            u32 r = (((p >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
            u32 g = (((p >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
            u32 b = ((p & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
            *dst = (r << 16) | (g << 8) | b;
        }
    }
}
static void blit_icon32(int x, int y, int idx) {
    if (idx < 0 || idx >= ICONC_COUNT || !icon_cache_ok[idx]) {
        ui_panel(x, y, 32, 32, 0);
        return;
    }
    blit_rgba32(x, y, icon_cache[idx]);
}

/* One desktop icon: bitmap glyph centered in its slot, two label lines
 * (name, then extension) centered underneath. */
static void draw_icon_with_label(int slot_x, int y, int idx, const char *line1, int n1, const char *line2, int n2) {
    blit_icon32(slot_x + (ICON_SLOT_W - ICON_GLYPH_W) / 2, y, idx);
    font_draw_string(slot_x + (ICON_SLOT_W - FONT_CELL * n1) / 2, y + ICON_GLYPH_H + 4, line1, TH_DESKTOP_TEXT);
    font_draw_string(slot_x + (ICON_SLOT_W - FONT_CELL * n2) / 2, y + ICON_GLYPH_H + 4 + FONT_CELL + 1, line2, TH_DESKTOP_TEXT);
}

static void draw_desktop_icon(void) {
    draw_icon_with_label(ICON_X, ICON_Y, ICONC_NOTEPAD, "NOTEPAD", 7, ".MWP", 4);
}

/* SETTING.MWP -- the second cell of the desktop grid, under NOTEPAD.MWP.
 * Double-clicking opens the real SYSTEM settings window (Language, IME)
 * defined further down, right alongside Notepad's own window code. */
#define ICON2_X  DESK_CELL_X(1)
#define ICON2_Y  DESK_CELL_Y(1)
#define ICON2_W  ICON_SLOT_W
#define ICON2_H  ICON_H

static void draw_desktop_icon2(void) {
    draw_icon_with_label(ICON2_X, ICON2_Y, ICONC_SETTING, "SETTING", 7, ".MWP", 4);
}

/* MINIWEB.MWP -- the third cell. Double-clicking opens
 * MiniWeb, the small HTTP client browser built on kernel/http.h and
 * kernel/tcp.h -- this OS's actual "layer above the network stack,"
 * not just a diagnostic harness proving the stack works. */
#define ICON3_X  DESK_CELL_X(2)
#define ICON3_Y  DESK_CELL_Y(2)
#define ICON3_W  ICON_SLOT_W
#define ICON3_H  ICON_H

static void draw_desktop_icon3(void) {
    draw_icon_with_label(ICON3_X, ICON3_Y, ICONC_WEB, "MINIWEB", 7, ".MWP", 4);
}

/* MINISHELL.MWP -- the fourth cell (it used to be
 * reachable from the Start Menu only, because Terminal.png hadn't been
 * supplied yet). Double-click opens the command-line window. */
#define ICON4_X  DESK_CELL_X(3)
#define ICON4_Y  DESK_CELL_Y(3)
#define ICON4_W  ICON_SLOT_W
#define ICON4_H  ICON_H

static void draw_desktop_icon4(void) {
    draw_icon_with_label(ICON4_X, ICON4_Y, ICONC_TERM, "MINISHELL", 9, ".MWP", 4);
}

/* MINIFILES.MWP -- the fifth cell (the file manager). Double-click opens it. "MINIFILES" is one glyph wider than the slot; the
 * label simply spills into the 10px column gap on each side, where nothing else draws. */
#define ICON5_X  DESK_CELL_X(4)
#define ICON5_Y  DESK_CELL_Y(4)
#define ICON5_W  ICON_SLOT_W
#define ICON5_H  ICON_H

static void draw_desktop_icon5(void) {
    draw_icon_with_label(ICON5_X, ICON5_Y, ICONC_FILES, "MINIFILES", 9, ".MWP", 4);
}

/* (TASKBAR_H / TASKBAR_Y come from ui/theme.h) */

#endif
