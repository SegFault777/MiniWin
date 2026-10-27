/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_APPS_NOTEPAD_MENUS_H
#define MW_APPS_NOTEPAD_MENUS_H

/* apps/notepad/menus.h -- Notepad's menu bar (File, Edit, Help), described as data for ui/menubar.h, and what each item does.
 * (Until alpha-11 only File worked, with its own hand-written drop-down; Edit and Help were labels.)
 *
 * THE EDITING MODEL, said plainly: Notepad has always typed at the END of the text -- there is no caret to move and no way to point
 * into the middle of it. A selection therefore has one useful shape: all of it. So *Select All* highlights the whole text, and Cut,
 * Copy and Delete work on it; typing or Backspace while it is selected replaces/deletes it, exactly as in any editor; Paste appends
 * the clipboard (replacing the text first when everything is selected). Undo keeps ONE snapshot -- the text as it was before the
 * last edit (a run of typing counts as one edit) -- and swaps it with the current text, so Undo twice is Redo.
 *
 * The clipboard is the OS one (clipboard.h), shared with programs. */

static const mb_menu_t np_menus[3] = {
    { { "File", "\xed\x8c\x8c\xec\x9d\xbc" }, 4, { { "New  Ctrl+N", "\xec\x83\x88\x20\xeb\xac\xb8\xec\x84\x9c\x20\x20\x43\x74\x72\x6c\x2b\x4e" }, { "Save  Ctrl+S", "\xec\xa0\x80\xec\x9e\xa5\x20\x20\x43\x74\x72\x6c\x2b\x53" }, { "Save As...", "\xeb\x8b\xa4\xeb\xa5\xb8\x20\xec\x9d\xb4\xeb\xa6\x84\xec\x9c\xbc\xeb\xa1\x9c\x20\xec\xa0\x80\xec\x9e\xa5" }, { "Close  Ctrl+W", "\xeb\x8b\xab\xea\xb8\xb0\x20\x20\x43\x74\x72\x6c\x2b\x57" } } },
    { { "Edit", "\xed\x8e\xb8\xec\xa7\x91" }, 7, { { "Undo  Ctrl+Z", "\xec\x8b\xa4\xed\x96\x89\x20\xec\xb7\xa8\xec\x86\x8c\x20\x20\x43\x74\x72\x6c\x2b\x5a" }, { "Cut  Ctrl+X", "\xec\x9e\x98\xeb\x9d\xbc\xeb\x82\xb4\xea\xb8\xb0\x20\x20\x43\x74\x72\x6c\x2b\x58" }, { "Copy  Ctrl+C", "\xeb\xb3\xb5\xec\x82\xac\x20\x20\x43\x74\x72\x6c\x2b\x43" }, { "Paste  Ctrl+V", "\xeb\xb6\x99\xec\x97\xac\xeb\x84\xa3\xea\xb8\xb0\x20\x20\x43\x74\x72\x6c\x2b\x56" }, { "Delete", "\xec\x82\xad\xec\xa0\x9c" }, { "Select All  Ctrl+A", "\xeb\xaa\xa8\xeb\x91\x90\x20\xec\x84\xa0\xed\x83\x9d\x20\x20\x43\x74\x72\x6c\x2b\x41" }, { "Time/Date", "\xec\x8b\x9c\xea\xb0\x84\x2f\xeb\x82\xa0\xec\xa7\x9c" } } },
    { { "Help", "\xeb\x8f\x84\xec\x9b\x80\xeb\xa7\x90" }, 2, { { "Keys", "\xeb\x8b\xa8\xec\xb6\x95\xed\x82\xa4" }, { "About Notepad", "\xeb\xa9\x94\xeb\xaa\xa8\xec\x9e\xa5\x20\xec\xa0\x95\xeb\xb3\xb4" } } },
};
#define NP_MENU_N 3

/* Settles an in-progress Hangul syllable into the text before anything edits it (same rule the typing code uses for a space or Enter). */
static void np_commit_ime(void) {
    if (current_ime == IME_KOREAN && ko_ime_is_composing()) ko_ime_commit(active_np->text_buf, &active_np->text_len, sizeof(active_np->text_buf));
}
/* Remember the text as it is now, for Edit > Undo. */
static void np_snapshot(void) {
    for (u32 i = 0; i < active_np->text_len; i++) active_np->undo_buf[i] = active_np->text_buf[i];
    active_np->undo_len = active_np->text_len; active_np->undo_valid = 1;
}
static void np_set_text(const char *s, u32 n) {
    if (n > sizeof(active_np->text_buf) - 1) n = sizeof(active_np->text_buf) - 1;
    for (u32 i = 0; i < n; i++) active_np->text_buf[i] = s[i];
    active_np->text_len = n; active_np->text_buf[n] = 0;
}
static int np_need_selection(void) {
    if (active_np->sel_all && active_np->text_len) return 1;
    status = "SELECT ALL FIRST (EDIT MENU OR CTRL+A)";
    return 0;
}

static void np_edit_undo(void) {
    if (!active_np->undo_valid) { status = "NOTHING TO UNDO"; return; }
    np_commit_ime();
    static char tmp[FS_MAX_FILE_BYTES];                    /* swap: the undone text becomes the new snapshot, so Undo again redoes */
    u32 n = active_np->text_len;
    for (u32 i = 0; i < n; i++) tmp[i] = active_np->text_buf[i];
    np_set_text(active_np->undo_buf, active_np->undo_len);
    for (u32 i = 0; i < n; i++) active_np->undo_buf[i] = tmp[i];
    active_np->undo_len = n;
    active_np->sel_all = 0; active_np->typing = 0;
    status = "UNDONE";
}
static void np_edit_copy(int cut) {
    np_commit_ime();
    if (!np_need_selection()) return;
    if (!clip_set(active_np->text_buf, active_np->text_len)) { status = "TOO BIG FOR THE CLIPBOARD"; return; }
    if (cut) { np_snapshot(); active_np->text_len = 0; active_np->text_buf[0] = 0; active_np->sel_all = 0; active_np->typing = 0; status = "CUT"; }
    else status = "COPIED";
}
static void np_edit_paste(void) {
    np_commit_ime();
    if (!clip_len) { status = "CLIPBOARD IS EMPTY"; return; }
    np_snapshot(); active_np->typing = 0;
    if (active_np->sel_all) { active_np->text_len = 0; active_np->text_buf[0] = 0; active_np->sel_all = 0; }   /* paste over a selection */
    u32 room = (u32)sizeof(active_np->text_buf) - 1 - active_np->text_len, n = clip_len < room ? clip_len : room;
    clip_get(active_np->text_buf + active_np->text_len, n);
    active_np->text_len += n; active_np->text_buf[active_np->text_len] = 0;
    status = n < clip_len ? "PASTED (DOCUMENT FULL: CUT SHORT)" : "PASTED";
}
static void np_edit_delete(void) {
    np_commit_ime();
    if (!np_need_selection()) return;
    np_snapshot(); active_np->text_len = 0; active_np->text_buf[0] = 0; active_np->sel_all = 0; active_np->typing = 0;
    status = "DELETED";
}
static void np_edit_select_all(void) {
    np_commit_ime();
    if (!active_np->text_len) { status = "NOTHING TO SELECT"; return; }
    active_np->sel_all = 1; active_np->typing = 0;
    status = "ALL SELECTED";
}
static void np_edit_time_date(void) {
    np_commit_ime();
    rtc_time_t now; rtc_read(&now);
    rtc_time_t local = rtc_apply_offset(now, tz_offset_hours);
    char s[40]; format_full_date(&local, s, sizeof(s));
    int n = 0; while (s[n]) n++;
    char hm[8]; hm[0] = ' '; hm[1] = (char)('0' + local.hour / 10); hm[2] = (char)('0' + local.hour % 10); hm[3] = ':'; hm[4] = (char)('0' + local.minute / 10); hm[5] = (char)('0' + local.minute % 10); hm[6] = 0;
    np_snapshot(); active_np->typing = 0;
    if (active_np->sel_all) { active_np->text_len = 0; active_np->text_buf[0] = 0; active_np->sel_all = 0; }
    for (int i = 0; i < n; i++) kstrcpy_append(active_np->text_buf, &active_np->text_len, sizeof(active_np->text_buf), s[i]);
    for (int i = 0; hm[i]; i++) kstrcpy_append(active_np->text_buf, &active_np->text_len, sizeof(active_np->text_buf), hm[i]);
    status = "DATE AND TIME INSERTED";
}

/* The chosen item of the menu bar; active_np is the window the menu belongs to. */
static void np_menu_action(int menu, int item) {
    np_commit_ime();
    if (menu == 0) {
        if (item == 0) { active_np->confirm_mode = CONFIRM_NEW; beep_warning(); }
        else if (item == 1) { int s = save_current_document(); status = s >= 0 ? format_saved_status(s) : save_error_status(s); }
        else if (item == 2) saveas_begin();
        else { active_np->confirm_mode = CONFIRM_CLOSE; beep_warning(); }
    } else if (menu == 1) {
        if (item == 0) np_edit_undo();
        else if (item == 1) np_edit_copy(1);
        else if (item == 2) np_edit_copy(0);
        else if (item == 3) np_edit_paste();
        else if (item == 4) np_edit_delete();
        else if (item == 5) np_edit_select_all();
        else np_edit_time_date();
    } else {
        active_np->confirm_mode = CONFIRM_INFO;
        active_np->info_kind = item == 0 ? 1 : 2;
    }
}

/* Ctrl+key accelerators. Returns 1 if the key was one of them. */
static int np_ctrl_key(char c) {
    if (c == 'z' || c == 'Z') np_edit_undo();
    else if (c == 'x' || c == 'X') np_edit_copy(1);
    else if (c == 'c' || c == 'C') np_edit_copy(0);
    else if (c == 'v' || c == 'V') np_edit_paste();
    else if (c == 'a' || c == 'A') np_edit_select_all();
    else return 0;
    return 1;
}

/* Called for every typed key BEFORE it edits the text: a selection is replaced (or, for Backspace, deleted) and the first key of a
 * run of typing takes the undo snapshot. Returns 1 when the key has been fully handled here (Backspace over a selection). */
static int np_before_typing(char c) {
    if (active_np->sel_all) {
        np_commit_ime();
        np_snapshot(); active_np->text_len = 0; active_np->text_buf[0] = 0; active_np->sel_all = 0; active_np->typing = 1;
        return c == '\b';
    }
    if (!active_np->typing) { np_snapshot(); active_np->typing = 1; }
    return 0;
}

#endif
