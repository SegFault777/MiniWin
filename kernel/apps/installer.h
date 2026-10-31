/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_APPS_INSTALLER_H
#define MW_APPS_INSTALLER_H

/* apps/installer.h -- the INSTALL window of the live disc (1.1-alpha-13).
 *
 * WHEN IT EXISTS. Only on a live boot (kernel/live.h: live_boot == 1, the OS running from a RAM copy of the disc image). Its desktop icon
 * (INSTALL.MWP, bottom right, ui/icons.h) is drawn only then, and a disk that MiniWin was installed on is not a live disc -- kernel/os_inst.h
 * zeroes the LIVE stamp in the boot sector it writes -- so an installed system never shows the icon.
 *
 * WHAT IT DOES. It is a four-step wizard around the engine the terminal's `os-inst <id> i386` already uses (kernel/os_inst.h):
 *     1. PICK     the disks and partitions that st_resolve() accepts (the disc and the RAM disk are never offered), the others greyed
 *                 out with the reason; a disk or a partition is a target
 *     2. CONFIRM  what is about to be erased, in words; the YES has to be TYPED (the same rule as the terminal: nothing destructive
 *                 happens on a click)
 *     3. INSTALL  inst_begin() + inst_step() a few sectors per frame, a progress bar (written, then read back and compared)
 *     4. DONE     what happened to the disk's boot sector, and a Restart button -- or, if it failed, which sector and why
 * The engine writes sector 0 LAST, so a window closed or a machine reset half-way leaves a disk that does not look bootable.
 *
 * HOW IT RUNS. Exactly like a program in a window (apps/progwin.h): a coroutine on its own stack that draws into the window's surface
 * and hands the CPU back once per frame with pw_yield(); the title bar, task bar button, minimise / maximise / close, focus and
 * keyboard routing all come from there. pw_launch_fn() starts a function that is part of the kernel instead of a file. While the window
 * owns a running install (inst_gui_owns) the terminal's poll leaves the job alone; if the window is closed in the middle the flag is
 * cleared (pw_free_run) and the terminal's poll simply finishes the install in the background.
 * Needs: apps/progwin.h, sys/power.h, ui/icon16.h, ui/widgets.h, os_inst.h, storage.h, terminal.h (term_fmt_size). */

#define INS_MAX_ROWS 24
#define INS_LINE_H   13

enum { INS_PICK, INS_CONFIRM, INS_RUN, INS_DONE, INS_FAIL };

typedef struct {
    char id[16];          /* "ide100" / "sata102" */
    char text[72];        /* what the list shows: id, size, model */
    int  ok;              /* st_resolve() accepts it */
    int  part;            /* a partition row (indented) */
    int  icon;            /* ui/icon16.h id */
    char why[72];         /* when !ok: the reason */
} ins_row_t;

static ins_row_t   ins_rows[INS_MAX_ROWS];
static int         ins_nrows, ins_page, ins_sel, ins_top, ins_prev_down, ins_quit;
static st_target_t ins_t;
static char        ins_info[7][64];        /* the confirm page's facts */
static int         ins_info_n;
static char        ins_yes[8];
static int         ins_yes_n;
static char        ins_msg[80];            /* a one-line notice (pick page: why the selection cannot be used; confirm page: why it cannot start) */

/* ---------------------------------------------------------------- the disks */
static void ins_add_row(int type, u32 num, int partnum, const char *what, u32 sectors) {
    if (ins_nrows >= INS_MAX_ROWS) return;
    ins_row_t *r = &ins_rows[ins_nrows];
    st_format_id((st_type_t)type, num, (u32)partnum, r->id);
    u32 l = 0;
    r->text[0] = 0;
    append_str(r->text, &l, sizeof r->text, r->id);
    append_str(r->text, &l, sizeof r->text, "   ");
    term_fmt_size(r->text, &l, sizeof r->text, sectors);
    append_str(r->text, &l, sizeof r->text, "   ");
    append_str(r->text, &l, sizeof r->text, what);
    r->part = partnum != 0;
    r->icon = r->part ? IC_DRIVE : type == ST_IDE ? IC_IDE : type == ST_SATA ? IC_SATA : type == ST_NVME ? IC_NVME : type == ST_SCSI ? IC_SCSI : type == ST_FD ? IC_FLOPPY : IC_DRIVE;
    st_target_t t;
    int e = st_resolve(r->id, &t);
    r->ok = (e == ST_OK);
    r->why[0] = 0;
    if (!r->ok) { const char *w = st_error_text(e); u32 k = 0; while (w[k] && k < sizeof r->why - 1) { r->why[k] = w[k]; k++; } r->why[k] = 0; }
    ins_nrows++;
}

static void ins_scan(void) {
    ins_nrows = 0; ins_sel = -1; ins_top = 0;
    if (!st_scanned) st_scan();
    for (int d = 0; d < BLK_DEV_MAX && ins_nrows < INS_MAX_ROWS; d++) {      /* the RAM disk is index BLK_DEV_MAX: never in this loop */
        if (!st_devs[d].present) continue;
        int type = blk_type_of(d);
        if (type == ST_CD) continue;                                         /* an optical disc cannot be written */
        u32 num = (u32)(d - blk_first_of(type)) + 1;
        ins_add_row(type, num, 0, st_devs[d].model[0] ? st_devs[d].model : "disk", st_devs[d].sectors);
        st_part_t parts[8];
        int n = st_list_partitions(d, parts, 8);
        for (int i = 0; i < n; i++) {
            if (parts[i].extended_container) continue;
            ins_add_row(type, num, parts[i].number, "partition", parts[i].sectors);
        }
    }
    for (int i = 0; i < ins_nrows; i++) if (ins_rows[i].ok) { ins_sel = i; break; }
}

/* ---------------------------------------------------------------- little helpers */
static int ins_cw(void) { return pw_cw(); }
static int ins_ch(void) { return pw_ch(); }

/* Draws `s` word-wrapped into width `w`; returns the y below the last line. */
static int ins_wrap(int x, int y, int w, const char *s, u32 col) {
    char tmp[100];
    int i = 0;
    while (s[i]) {
        while (s[i] == ' ') i++;
        if (!s[i]) break;
        int len = 0;
        for (;;) {
            int e = i + len;
            while (s[e] == ' ') e++;
            while (s[e] && s[e] != ' ') e++;
            if (e - i >= (int)sizeof tmp - 1) break;
            for (int k = 0; k < e - i; k++) tmp[k] = s[i + k];
            tmp[e - i] = 0;
            if (len > 0 && font_width_k(tmp) > w) break;
            len = e - i;
            if (!s[e]) break;
        }
        if (len == 0) len = 1;
        for (int k = 0; k < len; k++) tmp[k] = s[i + k];
        tmp[len] = 0;
        font_draw_k(x, y, tmp, col);
        y += INS_LINE_H; i += len;
    }
    return y;
}

/* `s` cut (in place) so that it fits `w` pixels. */
static void ins_fit(char *s, int w) {
    int n = 0; while (s[n]) n++;
    while (n > 0 && font_width_k(s) > w) s[--n] = 0;
}

/* A labelled button; returns 1 when it was clicked this frame. A disabled one is drawn flat-grey and never fires. */
static int ins_button(int x, int y, int w, int h, const char *label, int enabled, int mx, int my, int down, int click) {
    int over = in_rect(mx, my, x, y, w, h);
    int pressed = enabled && over && down;
    ui_bevel(x, y, w, h, pressed);
    int o = pressed ? 1 : 0;
    font_draw_k(x + (w - font_width_k(label)) / 2 + o, y + (h - FONT_CELL) / 2 + o, label, enabled ? TH_TEXT : TH_SHADOW);
    return enabled && over && click;
}

static void ins_header(const char *title) {
    int cw = ins_cw();
    ic16_blit(10, 8, IC_INSTALL);
    font_draw_k(32, 10, title, TH_TEXT);
    ui_hline(8, 28, cw - 16, TH_SHADOW);
    ui_hline(8, 29, cw - 16, TH_LIGHT);
}

/* ---------------------------------------------------------------- the pages */
static void ins_go_confirm(void) {
    ins_msg[0] = 0;
    if (ins_sel < 0 || !ins_rows[ins_sel].ok) return;
    int e = st_resolve(ins_rows[ins_sel].id, &ins_t);
    if (e != ST_OK) { const char *w = st_error_text(e); u32 k = 0; while (w[k] && k < sizeof ins_msg - 1) { ins_msg[k] = w[k]; k++; } ins_msg[k] = 0; return; }
    inst_probe_t pr;
    inst_probe(&ins_t, &pr);
    u32 l;
    ins_info_n = 0;
    l = 0; ins_info[ins_info_n][0] = 0;
    append_str(ins_info[ins_info_n], &l, 64, "Target: "); append_str(ins_info[ins_info_n], &l, 64, ins_t.id);
    append_str(ins_info[ins_info_n], &l, 64, ins_t.whole ? "  (the whole disk)" : "  (one partition)"); ins_info_n++;
    l = 0; ins_info[ins_info_n][0] = 0;
    append_str(ins_info[ins_info_n], &l, 64, st_devs[ins_t.pos].model[0] ? st_devs[ins_t.pos].model : "disk");
    append_str(ins_info[ins_info_n], &l, 64, ", "); term_fmt_size(ins_info[ins_info_n], &l, 64, ins_t.sectors); ins_info_n++;
    l = 0; ins_info[ins_info_n][0] = 0;
    append_str(ins_info[ins_info_n], &l, 64, "Starts at sector "); append_uint(ins_info[ins_info_n], &l, 64, (int)ins_t.base); ins_info_n++;
    l = 0; ins_info[ins_info_n][0] = 0;
    append_str(ins_info[ins_info_n], &l, 64, !pr.readable ? "Warning: it cannot be read first."
                                          : pr.miniwin ? "It already holds a MiniWin."
                                          : pr.non_empty ? "It holds data (it is not empty)." : "It looks empty."); ins_info_n++;
    if (ins_t.whole && pr.partitions > 0) {
        l = 0; ins_info[ins_info_n][0] = 0;
        append_str(ins_info[ins_info_n], &l, 64, "Its partition table (");
        append_uint(ins_info[ins_info_n], &l, 64, pr.partitions);
        append_str(ins_info[ins_info_n], &l, 64, ") will be destroyed."); ins_info_n++;
    }
    ins_yes_n = 0; ins_yes[0] = 0;
    ins_page = INS_CONFIRM;
}

static void ins_start(void) {
    if (inst_active() || term_inst_pending) { u32 l = 0; ins_msg[0] = 0; append_str(ins_msg, &l, sizeof ins_msg, "Another install is running (MiniShell)."); return; }
    inst_begin(&ins_t, st_boot_id);
    inst_gui_owns = 1;
    ins_page = INS_RUN;
}

static void ins_page_pick(int mx, int my, int down, int click, int k) {
    int cw = ins_cw(), ch = ins_ch();
    ins_header("Install MiniWin");
    int y = ins_wrap(12, 36, cw - 24, "Pick the disk or partition to install on. MiniWin is copied there and the filesystem on it is made new: everything on it is erased.", TH_TEXT);
    int lx = 10, ly = y + 6, lw = cw - 20, rh = 16;
    int lh = ch - ly - 62; if (lh < rh) lh = rh;
    int vis = lh / rh; if (vis < 1) vis = 1;
    lh = vis * rh + 4;
    ui_field(lx, ly, lw, lh, TH_FIELD, 1);

    if (k == KEY_DOWN && ins_sel < ins_nrows - 1) ins_sel++;
    else if (k == KEY_UP && ins_sel > 0) ins_sel--;
    if (ins_sel >= 0) { if (ins_sel < ins_top) ins_top = ins_sel; if (ins_sel >= ins_top + vis) ins_top = ins_sel - vis + 1; }

    if (click && in_rect(mx, my, lx + 2, ly + 2, lw - 4, vis * rh)) {
        int i = ins_top + (my - ly - 2) / rh;
        if (i >= 0 && i < ins_nrows) ins_sel = i;
    }
    if (ins_nrows == 0) font_draw_k(lx + 8, ly + 6, "No disk to install on was found.", TH_SHADOW);
    for (int i = 0; i < vis && ins_top + i < ins_nrows; i++) {
        ins_row_t *r = &ins_rows[ins_top + i];
        int ry = ly + 2 + i * rh, sel = (ins_top + i == ins_sel);
        if (sel) bb_fillrect(lx + 2, ry, lw - 4, rh, TH_ACCENT);
        int ix = lx + 6 + (r->part ? 14 : 0);
        ic16_blit(ix, ry, r->icon);
        char txt[72]; u32 n = 0; while (r->text[n] && n < sizeof txt - 1) { txt[n] = r->text[n]; n++; } txt[n] = 0;
        ins_fit(txt, lw - 12 - (ix - lx) - 20);
        font_draw_k(ix + 20, ry + 2, txt, sel ? TH_TEXT_INVERSE : (r->ok ? TH_TEXT : TH_SHADOW));
    }
    int ny = ly + lh + 4;
    if (ins_sel >= 0 && !ins_rows[ins_sel].ok) {
        char why[72]; u32 n = 0; while (ins_rows[ins_sel].why[n] && n < sizeof why - 1) { why[n] = ins_rows[ins_sel].why[n]; n++; } why[n] = 0;
        ins_fit(why, cw - 20);
        font_draw_k(10, ny, why, COL_RED);
    } else if (ins_msg[0]) font_draw_k(10, ny, ins_msg, COL_RED);

    int by = ch - 28;
    if (ins_button(10, by, 80, 20, "Rescan", 1, mx, my, down, click)) ins_scan();
    if (ins_button(cw - 180, by, 80, 20, "Close", 1, mx, my, down, click) || k == 27) ins_quit = 1;
    int can = ins_sel >= 0 && ins_rows[ins_sel].ok;
    if (ins_button(cw - 90, by, 80, 20, "Next >", can, mx, my, down, click) || (can && k == '\n')) ins_go_confirm();
}

static void ins_page_confirm(int mx, int my, int down, int click, int k) {
    int cw = ins_cw(), ch = ins_ch();
    ins_header("Ready to install");
    int y = 38;
    for (int i = 0; i < ins_info_n; i++) { font_draw_k(14, y, ins_info[i], TH_TEXT); y += INS_LINE_H + 2; }
    y += 6;
    ic16_blit(14, y, IC_WARNING);
    ins_wrap(38, y + 1, cw - 52, "Everything on it will be erased and replaced by a fresh MiniWin with an empty filesystem. This cannot be undone.", TH_TEXT);
    y += 4 * INS_LINE_H + 6;

    font_draw_k(14, y + 3, "Type YES to confirm:", TH_TEXT);
    int fx = 14 + font_width_k("Type YES to confirm:") + 8;
    ui_field(fx, y, 64, 18, TH_FIELD, 1);
    font_draw_k(fx + 4, y + 3, ins_yes, TH_FIELD_TEXT);
    int cx = fx + 4 + font_width_k(ins_yes);
    bb_fillrect(cx, y + 3, 1, FONT_CELL, TH_FIELD_TEXT);
    int yes = ins_yes[0] == 'Y' && ins_yes[1] == 'E' && ins_yes[2] == 'S' && ins_yes[3] == 0;

    if (k == '\b') { if (ins_yes_n > 0) ins_yes[--ins_yes_n] = 0; }
    else if (k >= 32 && k < 127 && ins_yes_n < 6) { ins_yes[ins_yes_n++] = (char)k; ins_yes[ins_yes_n] = 0; }

    if (ins_msg[0]) font_draw_k(14, y + 26, ins_msg, COL_RED);
    int by = ch - 28;
    if (ins_button(10, by, 80, 20, "< Back", 1, mx, my, down, click) || k == 27) { ins_page = INS_PICK; ins_msg[0] = 0; }
    if (ins_button(cw - 90, by, 80, 20, "Install", yes, mx, my, down, click) || (yes && k == '\n')) ins_start();
}

static void ins_page_run(int mx, int my, int down, int click) {
    int cw = ins_cw(), ch = ins_ch();
    int ph = inst_active() ? inst_step(24) : inst.phase;
    if (ph == INST_PHASE_DONE) { ins_page = INS_DONE; return; }
    if (ph == INST_PHASE_FAILED) { ins_page = INS_FAIL; return; }
    ins_header("Installing");
    int y = ins_wrap(14, 40, cw - 28, "Installing MiniWin. Do not turn the computer off and do not close this window.", TH_TEXT);
    u32 pct = inst_percent();
    const char *what = ph == INST_PHASE_WRITE ? "Writing the system" : ph == INST_PHASE_VERIFY ? "Reading it back and checking" : "Making the disk bootable";
    font_draw_k(14, y + 10, what, TH_TEXT);
    int bx = 14, by = y + 30, bw = cw - 28;
    ui_field(bx, by, bw, 20, TH_FIELD, 0);
    int fill = (int)(((u32)(bw - 4) * pct) / 100u);
    if (fill > 0) bb_fillrect(bx + 2, by + 2, fill, 16, TH_ACCENT);
    char num[8]; u32 l = 0; num[0] = 0; append_uint(num, &l, sizeof num, (int)pct); append_str(num, &l, sizeof num, "%");
    font_draw_k(bx + (bw - font_width_k(num)) / 2, by + 5, num, pct > 50 ? TH_TEXT_INVERSE : TH_TEXT);
    ins_button(cw - 90, ch - 28, 80, 20, "Close", 0, mx, my, down, click);
}

static void ins_page_done(int mx, int my, int down, int click, int k) {
    int cw = ins_cw(), ch = ins_ch();
    ins_header("Done");
    char line[64]; u32 l = 0; line[0] = 0;
    append_str(line, &l, sizeof line, "MiniWin is installed on "); append_str(line, &l, sizeof line, inst.t.id); append_str(line, &l, sizeof line, ".");
    int y = ins_wrap(14, 40, cw - 28, line, TH_TEXT) + 4;
    y = ins_wrap(14, y, cw - 28, "Everything was written and read back to check it.", TH_TEXT) + 4;
    y = ins_wrap(14, y, cw - 28, inst_mbr_text(inst.mbr_result), TH_TEXT) + 4;
    ins_wrap(14, y, cw - 28, "To use it, take this disc out (or put the new disk first in the BIOS boot order) and restart.", TH_TEXT);
    int by = ch - 28;
    if (ins_button(cw - 180, by, 80, 20, "Close", 1, mx, my, down, click) || k == 27) ins_quit = 1;
    if (ins_button(cw - 90, by, 80, 20, "Restart", 1, mx, my, down, click)) system_restart();
}

static void ins_page_fail(int mx, int my, int down, int click, int k) {
    int cw = ins_cw(), ch = ins_ch();
    ins_header("Install failed");
    ic16_blit(14, 40, IC_ERROR);
    int y = ins_wrap(38, 41, cw - 52, inst_error_text(inst.err), TH_TEXT) + 4;
    char line[48]; u32 l = 0; line[0] = 0;
    append_str(line, &l, sizeof line, "At sector "); append_uint(line, &l, sizeof line, (int)inst.err_lba);
    font_draw_k(38, y, line, TH_TEXT); y += INS_LINE_H + 6;
    ins_wrap(14, y, cw - 28, "The target disk is not bootable now; your other disks were not touched. You can pick it again and retry, or pick another disk.", TH_TEXT);
    int by = ch - 28;
    if (ins_button(10, by, 80, 20, "< Back", 1, mx, my, down, click)) { ins_page = INS_PICK; ins_scan(); }
    if (ins_button(cw - 90, by, 80, 20, "Close", 1, mx, my, down, click) || k == 27) ins_quit = 1;
}

/* One frame: read what the desktop routed here, draw the page. */
static void ins_frame(void) {
    int mx = pw.mx, my = pw.my, down = pw.mb & 1;
    int click = down && !ins_prev_down;
    ins_prev_down = down;
    int k = pw_pop_key();
    bb_fillrect(0, 0, ins_cw(), ins_ch(), TH_FACE);
    switch (ins_page) {
        case INS_PICK:    ins_page_pick(mx, my, down, click, k); break;
        case INS_CONFIRM: ins_page_confirm(mx, my, down, click, k); break;
        case INS_RUN:     ins_page_run(mx, my, down, click); break;
        case INS_DONE:    ins_page_done(mx, my, down, click, k); break;
        default:          ins_page_fail(mx, my, down, click, k); break;
    }
}

/* The window's entry point (what pw_launch_fn runs). */
static void installer_main(void) {
    ins_page = INS_PICK; ins_prev_down = 0; ins_quit = 0; ins_yes_n = 0; ins_yes[0] = 0; ins_msg[0] = 0;
    ins_scan();
    while (!ins_quit) { ins_frame(); pw_yield(); }
}

/* Opens the installer window; 0, or a sentence saying why not (the desktop shows it on the status line). */
static const char *installer_open(void) {
    if (!live_boot) return "The installer is only on the live disc.";
    return pw_launch_fn(installer_main, "INSTALL MINIWIN", IC_INSTALL);
}

#endif
