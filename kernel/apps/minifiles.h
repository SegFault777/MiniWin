/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_APPS_MINIFILES_H
#define MW_APPS_MINIFILES_H

/* apps/minifiles.h -- MINIFILES.MWP, the file manager (1.1-alpha-1).
 *
 * Two tabs:
 *   FILES  every document (the 4 Notepad slots) and every program (.mwp) on the disk, with size and health. A
 *          document opens in Notepad, a program runs, either can be deleted (two clicks: the first arms it).
 *          "Health" is real: fs_verify_slot()/prog_verify_slot() read the whole thing and check its CRC-32, so a
 *          file that would open as garbage is shown as DAMAGED instead of looking fine.
 *   DISKS  the IDE disks and their partitions with the id each one is called by (the argument of the terminal's
 *          `os-inst`). INSTALL does not install anything by itself: it opens MiniShell with `os-inst <id>` typed
 *          in, so the one place that asks for a confirmation (and prints the progress) stays the Terminal.
 *
 * Window plumbing follows Terminal's pattern exactly (window.h / windows.h / taskbar.h / compose.h / input.h
 * each know about WIN_ID_FILES); this file holds the window's own state, geometry, drawing and click handling.
 * Requires: storage.h, fs.h, the Notepad helpers (open_document_slot), mwp.h, term_* (to prefill os-inst). */

#define FILES_TAB_FILES 0
#define FILES_TAB_DISKS 1

#define FILES_MAX_ROWS   100     /* up to FS_MAX_FILES documents + PROG_MAX_SLOTS programs (kernel/fs.h) */
#define FILES_ROW_H      18      /* 16-pixel icon + 1 pixel above and below */
#define FILES_BTN_H      17
#define FILES_NAME_MAX   24

#define FILES_KIND_DOC   0
#define FILES_KIND_PROG  1
#define FILES_KIND_DISK  2     /* a whole device ("ide100") */
#define FILES_KIND_PART  3     /* a partition ("ide102") */

typedef struct {
    u8  kind;
    u8  slot;                  /* FILES: the fs/program slot; DISKS: ATA position */
    u8  bad;                   /* FILES: failed verification; DISKS: not installable */
    u32 size;                  /* FILES: bytes; DISKS: sectors */
    char name[FILES_NAME_MAX]; /* FILES: file name; DISKS: the storage id */
    char note[16];             /* short right-hand remark (RUNNING, EXTENDED, DAMAGED ...) */
} files_row_t;

/* ---- the 16x16 row icons (from the icon catalog: tools/install_icons.py), cached in a fixed high-memory area ---- */
#define FILES_ICO_DOC   0
#define FILES_ICO_PRG   1
#define FILES_ICO_DISK  2
#define FILES_ICO_PART  3
#define FILES_ICO_COUNT 4
static const char *const files_icon_names[FILES_ICO_COUNT] = { "DOC", "ASM_CODE", "IDE_DRIVE", "DRIVE" };
#define files_icon_cache ((u32 (*)[16 * 16])MW_FILES_ICON_ADDR)
static u8 files_icon_ok[FILES_ICO_COUNT];
static int files_icons_loaded = 0;

/* Decodes the four icons once (on the first time the window opens, not at boot): 4 catalog lookups + 8 sector reads. A
 * missing icon (an image built without install_icons.py) just means that row has no picture. */
static void files_icons_load(void) {
    if (files_icons_loaded) return;
    files_icons_loaded = 1;
    u8 raw[ICON_16_BYTES];
    for (int i = 0; i < FILES_ICO_COUNT; i++) {
        files_icon_ok[i] = 0;
        int slot = icon_find_by_name(files_icon_names[i]);
        if (slot < 0 || !icon_load_slot(slot, 0, raw)) continue;
        for (int k = 0; k < 16 * 16; k++) {
            u32 r = raw[k * 4], g = raw[k * 4 + 1], b = raw[k * 4 + 2], a = raw[k * 4 + 3];
            files_icon_cache[i][k] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        files_icon_ok[i] = 1;
    }
}

/* Alpha-blends a cached 16x16 icon at (x,y); same rules as blit_icon32() in ui/icons.h. */
static void blit_icon16(int x, int y, int idx) {
    if (idx < 0 || idx >= FILES_ICO_COUNT || !files_icon_ok[idx]) return;
    const u32 *src = files_icon_cache[idx];
    for (int j = 0; j < 16; j++) {
        int py = y + j;
        if (py < 0 || py >= VGA_HEIGHT) continue;
        for (int i = 0; i < 16; i++) {
            int px = x + i;
            if (px < 0 || px >= VGA_WIDTH) continue;
            u32 p = src[j * 16 + i], a = p >> 24;
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

static files_row_t files_rows[FILES_MAX_ROWS];
static int  files_row_count = 0;
static int  files_tab = FILES_TAB_FILES;
static int  files_sel = -1;           /* selected row, -1 = none */
static int  files_scroll = 0;         /* first visible row */
static int  files_armed = -1;         /* row whose DELETE has been clicked once (-1 = none) */
static int  files_last_click_row = -1;
static u32  files_last_click_tick = 0;
static char files_msg[56];
static int  files_btn_pressed = -1;   /* index of the button being held down, -1 = none */

/* ---------------------------------------------------------------- geometry */
static inline int files_btn_close_x(void) { return win_btn_close_x(&files_win, 2); }
static inline int files_btn_max_x(void)   { return win_btn_max_x(&files_win, 2); }
static inline int files_btn_min_x(void)   { return win_btn_min_x(&files_win, 2); }
static inline int files_btn_y(void)       { return win_btn_y(&files_win, 1); }

static int files_close_hit(int px, int py) { return in_rect(px, py, files_btn_close_x(), files_btn_y(), BTN_W, BTN_H); }
static int files_min_hit(int px, int py)   { return in_rect(px, py, files_btn_min_x(),   files_btn_y(), BTN_W, BTN_H); }
static int files_max_hit(int px, int py)   { return in_rect(px, py, files_btn_max_x(),   files_btn_y(), BTN_W, BTN_H); }

static int files_titlebar_drag_hit(int px, int py) {
    if (!in_rect(px, py, files_win.x + 1, files_win.y + 1, files_win.w - 2, TITLEBAR_H)) return 0;
    if (files_min_hit(px, py) || files_max_hit(px, py) || files_close_hit(px, py)) return 0;
    return 1;
}

#define FILES_TAB_W 66
static inline int files_tab_y(void)  { return files_win.y + TITLEBAR_H + MB_H + 4; }
static inline int files_tab_x(int i) { return files_win.x + 6 + i * (FILES_TAB_W + 4); }
static int files_tab_hit(int px, int py, int i) { return in_rect(px, py, files_tab_x(i), files_tab_y(), FILES_TAB_W, 18); }

static inline int files_status_y(void) { return files_win.y + files_win.h - 16; }
static inline int files_btnrow_y(void) { return files_status_y() - FILES_BTN_H - 4; }
static inline int files_list_x(void)   { return files_win.x + 6; }
static inline int files_list_y(void)   { return files_tab_y() + 22; }
static inline int files_list_w(void)   { return files_win.w - 12; }
static inline int files_list_h(void)   { return files_btnrow_y() - 4 - files_list_y(); }
static inline int files_visible_rows(void) { int n = (files_list_h() - 4) / FILES_ROW_H; return n < 1 ? 1 : n; }

#define FILES_BTN_PRIMARY 0   /* OPEN / RUN / INSTALL */
#define FILES_BTN_DELETE  1   /* files tab only */
#define FILES_BTN_REFRESH 2
static const int files_btn_w[3] = { 84, 70, 86 };
static inline int files_btn_x(int i) { int x = files_win.x + 6; for (int k = 0; k < i; k++) x += files_btn_w[k] + 6; return x; }
static int files_btn_present(int i) { return !(files_tab == FILES_TAB_DISKS && i == FILES_BTN_DELETE); }
/* the disks tab has no DELETE button, so REFRESH slides left into its place */
static inline int files_btn_slot_x(int i) { return (files_tab == FILES_TAB_DISKS && i == FILES_BTN_REFRESH) ? files_btn_x(FILES_BTN_DELETE) : files_btn_x(i); }

static int files_row_at(int px, int py) {
    if (!in_rect(px, py, files_list_x() + 2, files_list_y() + 2, files_list_w() - 4, files_list_h() - 4)) return -1;
    int r = files_scroll + (py - files_list_y() - 2) / FILES_ROW_H;
    return (r >= 0 && r < files_row_count) ? r : -1;
}

/* ---------------------------------------------------------------- model */
static void files_set_msg(const char *s) { kstrcpy(files_msg, s, sizeof(files_msg)); }

static void files_add(u8 kind, u8 slot, u8 bad, u32 size, const char *name, const char *note) {
    if (files_row_count >= FILES_MAX_ROWS) return;
    files_row_t *r = &files_rows[files_row_count++];
    r->kind = kind; r->slot = slot; r->bad = bad; r->size = size;
    kstrcpy(r->name, name, sizeof(r->name));
    kstrcpy(r->note, note, sizeof(r->note));
}

/* Rebuilds the row list for the current tab from the disk. Reads every file once (a verification pass), so call it
 * on opening, tab switches and after changes -- never per frame. */
static void files_refresh(void) {
    files_row_count = 0;
    files_armed = -1;
    if (files_tab == FILES_TAB_FILES) {
        for (int s = 0; s < FS_MAX_FILES; s++) {
            u32 len = 0;
            if (!fs_check_slot(s, &len)) continue;
            int ok = fs_verify_slot(s);
            files_add(FILES_KIND_DOC, (u8)s, (u8)!ok, len, fs_doc_name(s), ok ? "" : "DAMAGED");
        }
        for (int s = 0; s < PROG_MAX_SLOTS; s++) {
            u32 len = 0; char name[PROG_NAME_MAXLEN];
            if (!prog_check_slot(s, &len, 0, name)) continue;
            int ok = prog_verify_slot(s);
            files_add(FILES_KIND_PROG, (u8)s, (u8)!ok, len, name, ok ? "" : "DAMAGED");
        }
    } else {
        st_scan();
        for (int d = 0; d < BLK_DEV_MAX; d++) {
            if (!st_devs[d].present) continue;
            char id[16];
            st_type_t dtype = (st_type_t)blk_type_of(d);
            u32 dnum = (u32)(d - blk_first_of(dtype)) + 1;
            st_format_id(dtype, dnum, 0, id);
            int running_dev = (d == blk_boot_dev);
            files_add(FILES_KIND_DISK, (u8)d, (u8)running_dev, st_devs[d].sectors, id, running_dev ? "RUNNING" : "");
            st_part_t parts[8];
            int n = st_list_partitions(d, parts, 8);
            for (int i = 0; i < n; i++) {
                st_format_id(dtype, dnum, (u32)parts[i].number, id);
                const char *note = "";
                int bad = 0;
                if (parts[i].extended_container) { note = "EXTENDED"; bad = 1; }
                else if (parts[i].sectors < ST_INSTALL_SECTORS) { note = "TOO SMALL"; bad = 1; }
                else if (d == blk_boot_dev && st_overlap(parts[i].start, parts[i].sectors, blk_boot_base, ST_INSTALL_SECTORS)) { note = "RUNNING"; bad = 1; }
                files_add(FILES_KIND_PART, (u8)d, (u8)bad, parts[i].sectors, id, note);
            }
        }
    }
    if (files_sel >= files_row_count) files_sel = files_row_count - 1;
    if (files_scroll > 0 && files_scroll >= files_row_count) files_scroll = 0;
}

static void files_ensure_visible(void) {
    int v = files_visible_rows();
    if (files_sel < files_scroll) files_scroll = files_sel;
    if (files_sel >= files_scroll + v) files_scroll = files_sel - v + 1;
    if (files_scroll < 0) files_scroll = 0;
}

static void files_open_window(void) {
    files_icons_load();
    files_win.open = 1; files_win.minimized = 0;
    files_refresh();
    files_set_msg("");
    win_z_raise(WIN_ID_FILES);
}

static void files_switch_tab(int tab) {
    if (tab == files_tab) return;
    files_tab = tab; files_sel = -1; files_scroll = 0;
    files_refresh();
    files_set_msg("");
}

/* ---------------------------------------------------------------- actions */
static void files_fmt_size(char *out, u32 *l, u32 cap, const files_row_t *r) {
    if (r->kind == FILES_KIND_DOC || r->kind == FILES_KIND_PROG) {
        append_uint(out, l, cap, r->size);
        append_str(out, l, cap, " B");
    } else {
        u32 mb = r->size / 2048;
        if (mb >= 10240) { append_uint(out, l, cap, mb / 1024); append_str(out, l, cap, " GB"); }
        else { append_uint(out, l, cap, mb); append_str(out, l, cap, " MB"); }
    }
}

/* OPEN a document / RUN a program / INSTALL (opens the Terminal with the command typed in) */
static void files_primary_action(void) {
    if (files_sel < 0 || files_sel >= files_row_count) { files_set_msg("Select something first."); return; }
    files_row_t *r = &files_rows[files_sel];
    if (r->kind == FILES_KIND_DOC) {
        open_document_slot(r->slot);                       /* sets the desktop status line itself */
        files_set_msg(r->bad ? "That file is damaged." : "Opened in Notepad.");
    } else if (r->kind == FILES_KIND_PROG) {
        if (r->bad) { files_set_msg("That program is damaged."); return; }
        if (!mwp_run(r->slot)) { files_set_msg("Failed to load or run."); return; }
        render_frame(-1, -1, 0);                           /* the program drew over everything: repaint */
    } else {
        if (r->bad) {                                       /* say WHY, in a sentence */
            if (r->note[0] == 'R') files_set_msg("That is where MiniWin runs from.");
            else if (r->note[0] == 'E') files_set_msg("Pick a logical partition inside it.");
            else if (r->note[0] == 'T') files_set_msg("Too small: needs at least 1 MB.");
            else files_set_msg("Cannot install there.");
            return;
        }
        /* hand over to the Terminal: the command is typed, NOT run -- the YES confirmation lives there */
        term_input_len = 0; term_input_buf[0] = 0;
        const char *cmd = "os-inst ";
        for (u32 i = 0; cmd[i]; i++) kstrcpy_append(term_input_buf, &term_input_len, sizeof(term_input_buf), cmd[i]);
        for (u32 i = 0; r->name[i]; i++) kstrcpy_append(term_input_buf, &term_input_len, sizeof(term_input_buf), r->name[i]);
        term_win.open = 1; term_win.minimized = 0;
        win_z_raise(WIN_ID_TERMINAL);
        term_input_focused = 1;
        files_set_msg("Press Enter in MiniShell.");
    }
}

static void files_delete_action(void) {
    if (files_tab != FILES_TAB_FILES) return;
    if (files_sel < 0 || files_sel >= files_row_count) { files_set_msg("Select a file first."); return; }
    files_row_t *r = &files_rows[files_sel];
    if (files_armed != files_sel) {                        /* first click: arm */
        files_armed = files_sel;
        u32 l = 0; char m[56];
        append_str(m, &l, sizeof(m), "Click DELETE again: ");
        append_str(m, &l, sizeof(m), r->name);
        files_set_msg(m);
        return;
    }
    int ok;
    if (r->kind == FILES_KIND_DOC) {
        if (find_notepad_bound_to(r->slot) >= 0) { files_set_msg("It is open in Notepad."); files_armed = -1; return; }
        ok = fs_delete_slot(r->slot);
    } else {
        ok = prog_delete_slot(r->slot);
        desktop_refresh_programs();
    }
    int keep = files_sel;
    files_refresh();
    if (r->kind == FILES_KIND_DOC) { u32 l = 0; desktop_file_exists[r->slot] = fs_check_slot(r->slot, &l) ? 1 : 0; desktop_file_len[r->slot] = l; }
    files_sel = keep < files_row_count ? keep : files_row_count - 1;
    files_set_msg(ok ? "Deleted." : "The disk refused the delete.");
}

/* ---------------------------------------------------------------- input */
/* A click inside the window's content (the caller has already handled the title-bar buttons and dragging). */
static void files_click(int mx, int my, u32 now, u32 dbl_window) {
    for (int t = 0; t < 2; t++) if (files_tab_hit(mx, my, t)) { files_switch_tab(t); return; }
    for (int b = 0; b < 3; b++) {
        int x = files_btn_slot_x(b);
        if (!files_btn_present(b)) continue;
        if (in_rect(mx, my, x, files_btnrow_y(), files_btn_w[b], FILES_BTN_H)) {
            files_btn_pressed = b;                          /* the action fires on release, over the same button */
            return;
        }
    }
    int r = files_row_at(mx, my);
    if (r >= 0) {
        if (files_armed != r) files_armed = -1;
        files_sel = r;
        if (r == files_last_click_row && (now - files_last_click_tick) < dbl_window && files_tab == FILES_TAB_FILES) {
            files_last_click_row = -1;
            files_primary_action();                         /* double-click = open / run */
        } else {
            files_last_click_row = r; files_last_click_tick = now;
        }
        return;
    }
    files_armed = -1;
}

/* Mouse released: fire the button that was pressed if the cursor is still over it. */
static void files_release(int mx, int my) {
    int b = files_btn_pressed;
    files_btn_pressed = -1;
    if (b < 0) return;
    if (!in_rect(mx, my, files_btn_slot_x(b), files_btnrow_y(), files_btn_w[b], FILES_BTN_H)) return;
    if (b == FILES_BTN_PRIMARY) files_primary_action();
    else if (b == FILES_BTN_DELETE) files_delete_action();
    else { files_refresh(); files_set_msg("Refreshed."); }
}

/* Keyboard (the window has focus): Up/Down select, Left/Right switch tabs, Enter = primary action. */
static void files_key(int k) {
    if (k == KEY_UP && files_sel > 0) { files_sel--; files_armed = -1; files_ensure_visible(); }
    else if (k == KEY_DOWN && files_sel < files_row_count - 1) { files_sel++; files_armed = -1; files_ensure_visible(); }
    else if (k == KEY_LEFT) files_switch_tab(FILES_TAB_FILES);
    else if (k == KEY_RIGHT) files_switch_tab(FILES_TAB_DISKS);
    else if (k == '\n') files_primary_action();
}

/* ---------------------------------------------------------------- drawing */
static void files_draw_button(int b, const char *label) {
    if (!files_btn_present(b)) return;
    int x = files_btn_slot_x(b);
    ui_button(x, files_btnrow_y(), files_btn_w[b], FILES_BTN_H, label, 6, 3, files_btn_pressed == b);
}


/* ---------------------------------------------------------------- MiniFiles' menu bar: what to do with the selection, which list, help */
#define FILES_MENU_N 3
static const mb_menu_t files_menus[FILES_MENU_N] = {
    { { "File", "\xed\x8c\x8c\xec\x9d\xbc" }, 4, { { "Open / Run / Install", "\xec\x97\xb4\xea\xb8\xb0 / \xec\x8b\xa4\xed\x96\x89 / \xec\x84\xa4\xec\xb9\x98" }, { "Delete", "\xec\x82\xad\xec\xa0\x9c" }, { "Refresh", "\xec\x83\x88\xeb\xa1\x9c\xea\xb3\xa0\xec\xb9\xa8" }, { "Close window", "\xec\xb0\xbd \xeb\x8b\xab\xea\xb8\xb0" } } },
    { { "View", "\xeb\xb3\xb4\xea\xb8\xb0" }, 2, { { "Files", "\xed\x8c\x8c\xec\x9d\xbc" }, { "Disks", "\xeb\x94\x94\xec\x8a\xa4\xed\x81\xac" } } },
    { { "Help", "\xeb\x8f\x84\xec\x9b\x80\xeb\xa7\x90" }, 1, { { "About MiniFiles", "MiniFiles \xec\xa0\x95\xeb\xb3\xb4" } } },
};
static void files_menu_action(int menu, int item) {
    if (menu == 0) {
        if (item == 0) files_primary_action();
        else if (item == 1) { if (files_tab == FILES_TAB_FILES) files_delete_action(); }
        else if (item == 2) { files_refresh(); files_set_msg("Refreshed"); }
        else { files_win.open = 0; win_z_remove(WIN_ID_FILES); }
    } else if (menu == 1) {
        files_switch_tab(item == 0 ? FILES_TAB_FILES : FILES_TAB_DISKS);
    } else {
        files_set_msg("MiniFiles: your documents, programs and disks");
    }
}

static void draw_files_window(void) {
    int wx = files_win.x, wy = files_win.y, ww = files_win.w, wh = files_win.h;
    ui_window_frame(wx, wy, ww, wh, files_win.maximized, "MINIFILES.MWP");
    ui_titlebar_buttons(files_btn_min_x(), files_btn_max_x(), files_btn_close_x(), files_btn_y(),
                        pressed_btn_kind == BTN_MIN && pressed_btn_win == WIN_ID_FILES,
                        pressed_btn_kind == BTN_MAX && pressed_btn_win == WIN_ID_FILES,
                        pressed_btn_kind == BTN_CLOSE && pressed_btn_win == WIN_ID_FILES);
    mb_draw_bar(WIN_ID_FILES, wx, wy, ww, files_menus, FILES_MENU_N);

    /* tabs */
    static const char *const tab_names[2] = { "FILES", "DISKS" };
    for (int t = 0; t < 2; t++) {
        int on = (t == files_tab);
        u32 fg = ui_item(files_tab_x(t), files_tab_y(), FILES_TAB_W, 18, on);
        font_draw_string(files_tab_x(t) + 8, files_tab_y() + 4, tab_names[t], fg);
    }

    /* list */
    int lx = files_list_x(), ly = files_list_y(), lw = files_list_w(), lh = files_list_h();
    ui_field(lx, ly, lw, lh, TH_FIELD, 0);
    int vis = files_visible_rows();
    if (files_row_count == 0) {
        font_draw_string(lx + 6, ly + 4, files_tab == FILES_TAB_FILES ? "(no files yet - save one in Notepad)" : "(no ide disk found)", TH_FIELD_TEXT);
    }
    for (int i = 0; i < vis && files_scroll + i < files_row_count; i++) {
        int ri = files_scroll + i;
        files_row_t *r = &files_rows[ri];
        int ry = ly + 2 + i * FILES_ROW_H;
        int sel = (ri == files_sel);
        u32 fg = TH_FIELD_TEXT;
        if (sel) { bb_fillrect(lx + 2, ry, lw - 4, FILES_ROW_H, TH_ACCENT); fg = TH_TEXT_INVERSE; }
        /* icon (partitions are indented one step under their disk), then name | size | kind | remark */
        int ico = r->kind == FILES_KIND_DOC ? FILES_ICO_DOC : r->kind == FILES_KIND_PROG ? FILES_ICO_PRG
                : r->kind == FILES_KIND_DISK ? FILES_ICO_DISK : FILES_ICO_PART;
        int ix = lx + 4 + (r->kind == FILES_KIND_PART ? 16 : 0);
        blit_icon16(ix, ry + 1, ico);
        int tx = ix + 20, ty = ry + (FILES_ROW_H - FONT_CELL) / 2;
        /* fixed columns, so the partitions' indent does not push the rest of the row around */
        int col_size = lx + 24 + 13 * FONT_CELL, col_kind = col_size + 8 * FONT_CELL, col_note = col_kind + 5 * FONT_CELL;
        char nm[16]; u32 nl = 0;
        for (u32 k = 0; r->name[k] && k < 12; k++) nm[nl++] = r->name[k];       /* names are cut to the column */
        nm[nl] = 0;
        font_draw_string(tx, ty, nm, fg);
        char sz[24]; u32 sl = 0;
        files_fmt_size(sz, &sl, sizeof(sz), r);
        font_draw_string(col_size, ty, sz, fg);
        const char *kind = r->kind == FILES_KIND_DOC ? "DOC" : r->kind == FILES_KIND_PROG ? "PRG" : r->kind == FILES_KIND_DISK ? "DISK" : "PART";
        font_draw_string(col_kind, ty, kind, fg);
        if (r->note[0]) font_draw_string(col_note, ty, r->note, (sel || !r->bad) ? fg : TH_ACCENT);
    }
    if (files_row_count > vis) sb_draw(WIN_ID_FILES);         /* the scroll bar (ui/scrollbar.h) */

    /* buttons: label depends on what is selected */
    const char *primary = "OPEN";
    if (files_tab == FILES_TAB_DISKS) primary = "INSTALL";
    else if (files_sel >= 0 && files_sel < files_row_count && files_rows[files_sel].kind == FILES_KIND_PROG) primary = "RUN";
    files_draw_button(FILES_BTN_PRIMARY, primary);
    files_draw_button(FILES_BTN_DELETE, files_armed >= 0 ? "SURE?" : "DELETE");
    files_draw_button(FILES_BTN_REFRESH, "REFRESH");

    /* status line */
    char st[64]; u32 sl = 0;
    if (files_msg[0]) append_str(st, &sl, sizeof(st), files_msg);
    else if (files_tab == FILES_TAB_DISKS) append_str(st, &sl, sizeof(st), "INSTALL asks in MiniShell: os-inst ?");
    else { append_uint(st, &sl, sizeof(st), (u32)files_row_count); append_str(st, &sl, sizeof(st), files_row_count == 1 ? " file" : " files"); }
    font_draw_string(wx + 6, files_status_y(), st, TH_TEXT);
    mb_draw_drop(WIN_ID_FILES, wx, wy, files_menus, FILES_MENU_N);
}

#endif
