#ifndef MW_UI_TASKBAR_H
#define MW_UI_TASKBAR_H

/* ui/taskbar.h -- taskbar drawing and hit-testing (pills, glyphs, Start button, clock slot).
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Taskbar drawing + hit-testing
 * ============================================================ */
/* Every minimized window gets a pill, ordered by WHEN it was minimized
 * (not by which app it is) -- so if you minimize Setting, then a
 * Notepad, the Setting pill sits to the left, exactly matching the
 * order the taskbar filled up in. Pills shrink to fit as more windows
 * pile up, the same way real taskbars do, instead of overflowing the
 * screen. */
#define TASKBTN_GAP    5
#define TASKBTN_MAXW   96
#define TASKBTN_MINW   48

/* Single source of truth for pill layout, shared by draw_taskbar() and
 * every taskbar hit-test below -- computing it twice with two different
 * formulas would be exactly how these things quietly drift out of sync. */
static void taskbar_layout(int *ids_out, int *count_out, int *pill_w_out) {
    int n = 0;
    for (int id = 0; id < WIN_ID_COUNT; id++) {
        if (win_is_open(id) && win_is_minimized(id)) ids_out[n++] = id;
    }
    /* insertion sort by minimize_seq ascending -- earliest-minimized
     * first. n is at most WIN_ID_COUNT (5), so this is plenty fast. */
    for (int i = 1; i < n; i++) {
        int key = ids_out[i];
        u32 kseq = minimize_seq[key];
        int j = i - 1;
        while (j >= 0 && minimize_seq[ids_out[j]] > kseq) {
            ids_out[j + 1] = ids_out[j];
            j--;
        }
        ids_out[j + 1] = key;
    }
    int avail_w = VGA_WIDTH - TASKBTN_X - 4 - CLOCK_W - 4; /* leave room for the clock */
    int pw = (n > 0) ? avail_w / n : TASKBTN_MAXW;
    if (pw > TASKBTN_MAXW) pw = TASKBTN_MAXW;
    if (pw < TASKBTN_MINW) pw = TASKBTN_MINW;
    *count_out = n;
    *pill_w_out = pw;
}

/* "N1...", "N2...", ..., or "S..." -- short enough to always fit even
 * the narrowest pill width. */
static void taskbar_pill_label(int id, char *out, u32 outsz) {
    u32 len = 0;
    if (id == WIN_ID_SETTING) {
        const char *s = "S...";
        while (*s) kstrcpy_append(out, &len, outsz, *s++);
    } else if (id == WIN_ID_WEB) {
        const char *s = "W...";
        while (*s) kstrcpy_append(out, &len, outsz, *s++);
    } else if (id == WIN_ID_TERMINAL) {
        const char *s = "T...";
        while (*s) kstrcpy_append(out, &len, outsz, *s++);
    } else {
        kstrcpy_append(out, &len, outsz, 'N');
        kstrcpy_append(out, &len, outsz, (char)('1' + id));
        const char *s = "...";
        while (*s) kstrcpy_append(out, &len, outsz, *s++);
    }
}

static void draw_taskbar(void) {
    bb_fillrect(0, TASKBAR_Y, VGA_WIDTH, TASKBAR_H, TH_FACE);
    ui_hline(0, TASKBAR_Y, VGA_WIDTH, TH_LIGHT);

    /* Start button. The classic "raised" 3D bevel is just a light stripe
     * on the top/left edges and a dark stripe on the bottom/right --
     * and flipping which stripe goes where makes it look "pressed in"
     * while the menu is open. This exact four-line trick single-handedly
     * carried the entire aesthetic of 16-bit UI toolkits. We salute it. */
    ui_bevel(STARTBTN_X, STARTBTN_Y, STARTBTN_W, STARTBTN_H, start_menu_open);
    /* nudge the label a pixel down-right while "pressed", like it's
     * physically sinking into the taskbar under the weight of your click */
    int press = start_menu_open ? 1 : 0;
    font_draw_string(STARTBTN_X + 5 + press, STARTBTN_Y + 2 + press, "AM", TH_TEXT);

    int ids[WIN_ID_COUNT], count, pill_w;
    taskbar_layout(ids, &count, &pill_w);
    for (int i = 0; i < count; i++) {
        int id = ids[i];
        int px = TASKBTN_X + i * (pill_w + TASKBTN_GAP);
        int py = TASKBTN_Y;

        ui_pill(px, py, pill_w, TASKBTN_H);
        char label[8];
        taskbar_pill_label(id, label, sizeof(label));
        font_draw_string(px + 3, py + 2, label, TH_TEXT);

        /* restore glyph: two overlapping squares */
        int rx = px + pill_w - 26, ry = py + 2;
        bb_rect(rx + 3, ry, 8, 8, TH_GLYPH_ON_FIELD);
        bb_rect(rx, ry + 3, 8, 8, TH_GLYPH_ON_FIELD);
        bb_fillrect(rx + 1, ry + 4, 6, 6, TH_FIELD);

        /* close glyph: X */
        int cx = px + pill_w - 13, cy = py + 2;
        for (int k = 0; k < 10; k++) {
            bb_putpixel(cx + k, cy + k, TH_GLYPH_ON_FIELD);
            bb_putpixel(cx + k, cy + 9 - k, TH_GLYPH_ON_FIELD);
        }
    }

    /* Clock, bottom-right corner. A sunken (rather than raised) look --
     * opposite bevel from the Start button -- since it's a readout, not
     * a button; pressed-looking while its date popup is open, same
     * "flip which edge is light" trick as everywhere else in this UI. */
    ui_readout(CLOCK_X, CLOCK_Y, CLOCK_W, CLOCK_H, clock_popup_open);
    {
        rtc_time_t now;
        rtc_read(&now);
        rtc_time_t local = rtc_apply_offset(now, tz_offset_hours);
        char clock_str[24];
        format_clock_time(&local, clock_str, sizeof(clock_str));
        int tw = ko_string_width(clock_str);
        ko_draw_mixed_string(CLOCK_X + (CLOCK_W - tw) / 2, CLOCK_Y + 1, clock_str, TH_TEXT);
    }
}

static int start_button_hit(int px, int py) {
    return in_rect(px, py, STARTBTN_X, STARTBTN_Y, STARTBTN_W, STARTBTN_H);
}

/* Returns the window id whose RESTORE glyph (kind=0) or CLOSE glyph
 * (kind=1) contains (px,py), using the exact same layout draw_taskbar()
 * just drew -- or -1 if the click didn't land on any pill's glyph. */
static int taskbar_glyph_hit(int px, int py, int kind) {
    int ids[WIN_ID_COUNT], count, pill_w;
    taskbar_layout(ids, &count, &pill_w);
    for (int i = 0; i < count; i++) {
        int bx = TASKBTN_X + i * (pill_w + TASKBTN_GAP);
        int gx = (kind == 0) ? (bx + pill_w - 26) : (bx + pill_w - 13);
        int gy = TASKBTN_Y + 2;
        if (in_rect(px, py, gx, gy, 11, 11)) return ids[i];
    }
    return -1;
}

#endif
