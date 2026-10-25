/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_APPS_NOTEPAD_SAVE_H
#define MW_APPS_NOTEPAD_SAVE_H

/* apps/notepad/save.h -- save / save-as / new logic shared by the menu, Ctrl+S and the confirm dialog.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Shared save logic -- used by File>Save, Ctrl+S, and the Yes actions of
 * both the "New" and "close window" confirm dialogs, so saving behaves
 * identically no matter which UI path triggered it.
 * ============================================================ */

/* Builds "SAVED: <filename>" (or its Korean equivalent) into status_buf
 * and returns it. */
static const char *format_saved_status(int slot) {
    u32 i = 0;
    const char *prefix = t(STR_SAVED_PREFIX);
    while (*prefix) status_buf[i++] = *prefix++;
    const char *name = fs_doc_name(slot);
    while (*name && i < sizeof(status_buf) - 1) status_buf[i++] = *name++;
    status_buf[i] = 0;
    return status_buf;
}

#define SAVE_ERR_FULL   (-1)   /* all FS_MAX_FILES slots are occupied */
#define SAVE_ERR_WRITE  (-2)   /* the save itself failed: disk error, or the text is bigger than a file can be */

/* The status-bar text for a failed save_current_document(): "disk full" and "the write failed" are different
 * problems and used to share one (misleading) message. */
static const char *save_error_status(int code) {
    return code == SAVE_ERR_WRITE ? t(STR_SAVE_FAILED) : t(STR_STORAGE_FULL_NOT_SAVED);
}

/* Saves active_np->text_buf/active_np->text_len to the slot the current document is bound to,
 * or to the first empty slot if unbound (and binds to it, so subsequent
 * saves of the same still-open document update that slot instead of
 * creating a new file each time). Returns the slot saved to, SAVE_ERR_FULL if
 * all FS_MAX_FILES slots are already occupied ("disk full"), or SAVE_ERR_WRITE if
 * the write did not succeed (the document stays open and unsaved -- see confirm_yes_action()). */
static int save_current_document(void) {
    int slot = active_np->bound_slot;
    if (slot < 0) {
        slot = fs_find_empty_slot();
        if (slot < 0) return SAVE_ERR_FULL;
    }
    if (!fs_save_slot(slot, active_np->text_buf, active_np->text_len)) {
        /* Whatever the failed save left behind (the old file, or a journal that recovery has since
         * completed), the desktop icon must match what is really on disk. */
        u32 l = 0;
        desktop_file_exists[slot] = fs_check_slot(slot, &l) ? 1 : 0;
        desktop_file_len[slot] = l;
        return SAVE_ERR_WRITE;
    }
    desktop_file_exists[slot] = 1;
    desktop_file_len[slot] = active_np->text_len;
    active_np->bound_slot = slot;
    return slot;
}

/* Which notepads[] slot (if any) is currently closed and free to reuse
 * for a newly-opened window -- either a blank "New" document or a
 * double-clicked file. Returns -1 if all NOTEPAD_MAX are already open. */
static int find_free_notepad_slot(void) {
    for (int i = 0; i < NOTEPAD_MAX; i++) {
        if (!notepads[i].win.open) return i;
    }
    return -1;
}

/* Is a given FS file slot already open in one of the notepad windows?
 * Used so double-clicking a desktop file icon that's already open just
 * focuses the existing window instead of loading a second, independently
 * editable copy of the same file (which would just race on Save). */
static int find_notepad_bound_to(int fs_slot) {
    for (int i = 0; i < NOTEPAD_MAX; i++) {
        if (notepads[i].win.open && notepads[i].bound_slot == fs_slot) return i;
    }
    return -1;
}

/* Opens the document in FS slot `slot` in a Notepad window (shared by the desktop file icons and MiniFiles). If it is
 * already open in some window that window is focused instead -- loading a second editable copy would race on Save --
 * and a damaged file is refused rather than opened as truncated text that the next Save would write over the original.
 * Sets the status line; returns 1 if a window now shows the file. */
static int open_document_slot(int slot) {
    int existing = find_notepad_bound_to(slot);
    if (existing >= 0) {
        notepads[existing].win.minimized = 0;
        win_z_raise(existing);
        status = format_saved_status(slot);
        return 1;
    }
    int free_slot = find_free_notepad_slot();
    if (free_slot < 0) { status = t(STR_ALL_NOTEPAD_WINDOWS_OPEN); return 0; }
    u32 loaded = 0;
    if (!fs_read_slot(slot, notepads[free_slot].text_buf, sizeof(notepads[free_slot].text_buf) - 1, &loaded)) {
        status = t(STR_FILE_DAMAGED);
        return 0;
    }
    active_np = &notepads[free_slot];
    active_np->text_buf[loaded] = 0;
    active_np->text_len = loaded;
    active_np->bound_slot = slot;
    active_np->win.open = 1;
    active_np->win.minimized = 0;
    active_np->file_menu_open = 0;
    active_np->confirm_mode = CONFIRM_NONE;
    ko_ime_reset();
    win_z_raise(free_slot);
    status = format_saved_status(slot);    /* reuse "SAVED: name" wording to show which file opened */
    return 1;
}

/* Shared Yes/No handling for the confirm dialog, used by both mouse
 * clicks and the Y/N keyboard shortcuts. What "Yes"/"No" actually do
 * depends on why the dialog was opened (active_np->confirm_mode). */
/* ---- Save As ---- */
/* Opens the dialog, suggesting the document's current name (or DOC.TXT for one that was never saved). */
static void saveas_begin(void) {
    if (ko_ime_is_composing()) ko_ime_commit(active_np->text_buf, &active_np->text_len, sizeof(active_np->text_buf));
    const char *suggest = active_np->bound_slot >= 0 ? fs_doc_name(active_np->bound_slot) : "DOC.TXT";
    int n = 0;
    while (suggest[n] && n < FS_NAME_MAXLEN) { active_np->saveas_name[n] = suggest[n]; n++; }
    active_np->saveas_name[n] = 0;
    active_np->saveas_len = n;
    active_np->confirm_mode = CONFIRM_SAVEAS;
}

/* The dialog's Save: validates the name, finds the slot (the document's own if the name is unchanged, a free one for a new
 * name -- Save As makes a COPY and leaves the original alone), writes it and rebinds the window to the new document.
 * A bad or taken name keeps the dialog open so the name can be fixed; a full disk or a failed write closes it with a message. */
static void saveas_do(void) {
    const char *name = active_np->saveas_name;
    if (!fs_name_ok(name)) { status = t(STR_BAD_FILE_NAME); return; }
    int existing = fs_find_by_name(name), slot;
    if (existing >= 0 && existing == active_np->bound_slot) slot = existing;
    else if (existing >= 0) { status = t(STR_NAME_EXISTS); return; }
    else {
        slot = fs_find_empty_slot();
        if (slot < 0) { status = t(STR_STORAGE_FULL_NOT_SAVED); active_np->confirm_mode = CONFIRM_NONE; return; }
    }
    if (!fs_save_slot_as(slot, name, active_np->text_buf, active_np->text_len)) {
        u32 l = 0;
        desktop_file_exists[slot] = fs_check_slot(slot, &l) ? 1 : 0;
        desktop_file_len[slot] = l;
        status = t(STR_SAVE_FAILED);
        active_np->confirm_mode = CONFIRM_NONE;
        return;
    }
    desktop_file_exists[slot] = 1;
    desktop_file_len[slot] = active_np->text_len;
    active_np->bound_slot = slot;
    status = format_saved_status(slot);
    active_np->confirm_mode = CONFIRM_NONE;
}

static void confirm_yes_action(void) {
    if (active_np->confirm_mode == CONFIRM_SAVEAS) { saveas_do(); return; }
    int saved_slot = save_current_document();
    if (saved_slot == SAVE_ERR_WRITE) {
        /* The text could not be written. "Yes, save it, then close/clear" must NOT go on to close/clear:
         * that would throw away the only copy. Keep the window and the text, say what happened. */
        status = t(STR_SAVE_FAILED);
        active_np->confirm_mode = CONFIRM_NONE;
        return;
    }
    if (active_np->confirm_mode == CONFIRM_NEW) {
        active_np->text_len = 0; active_np->text_buf[0] = 0; active_np->bound_slot = -1; ko_ime_reset();
        status = saved_slot >= 0 ? format_saved_status(saved_slot) : save_error_status(saved_slot);
    } else if (active_np->confirm_mode == CONFIRM_CLOSE) {
        active_np->win.open = 0;
        active_np->win.minimized = 0;
        win_z_remove(active_np->id);
        /* Closing ends this editing session -- clear the buffer so the
         * next time the app icon is double-clicked, it starts blank
         * rather than showing this document's leftover text again. */
        active_np->text_len = 0; active_np->text_buf[0] = 0; active_np->bound_slot = -1; ko_ime_reset();
        status = saved_slot >= 0 ? t(STR_SAVED_AND_CLOSED) : t(STR_STORAGE_FULL_CLOSED_NOT_SAVED);
    }
    active_np->confirm_mode = CONFIRM_NONE;
}

static void confirm_no_action(void) {
    if (active_np->confirm_mode == CONFIRM_SAVEAS) {            /* Cancel: nothing is discarded, the document stays as it was */
        active_np->confirm_mode = CONFIRM_NONE;
        status = t(STR_SAVE_AS_CANCELLED);
        return;
    }
    if (active_np->confirm_mode == CONFIRM_NEW) {
        active_np->text_len = 0; active_np->text_buf[0] = 0; active_np->bound_slot = -1; ko_ime_reset();
        status = t(STR_NEW_DOCUMENT_NOT_SAVED);
    } else if (active_np->confirm_mode == CONFIRM_CLOSE) {
        active_np->win.open = 0;
        active_np->win.minimized = 0;
        win_z_remove(active_np->id);
        /* Same as above: discard the in-memory buffer on close so a
         * fresh app-icon open doesn't resurrect this session's text. */
        active_np->text_len = 0; active_np->text_buf[0] = 0; active_np->bound_slot = -1; ko_ime_reset();
        status = t(STR_CLOSED_NOT_SAVED);
    }
    active_np->confirm_mode = CONFIRM_NONE;
}

/* Window-relative button positions, computed from current geometry. */
static inline int btn_close_x(void) { return win_btn_close_x(&active_np->win, 2); }
static inline int btn_max_x(void)   { return win_btn_max_x(&active_np->win, 2); }
static inline int btn_min_x(void)   { return win_btn_min_x(&active_np->win, 2); }
static inline int btn_y(void)       { return win_btn_y(&active_np->win, 1); }

static inline int edit_x(void) { return active_np->win.x + 5; }
static inline int edit_y(void) { return active_np->win.y + TITLEBAR_H + FONT_CELL + 6; }
static inline int edit_w(void) { return active_np->win.w - 10; }
static inline int edit_h(void) { return active_np->win.h - TITLEBAR_H - FONT_CELL - 10; }

/* ---- the text area's wrapping, in ONE place so drawing, the scroll bar and the follow-the-typing logic cannot disagree ----
 * Text wraps at the edge of the area minus the scroll bar. Returns how many rows the text occupies (the row the cursor is in
 * counts, so an empty note has 1); *end_rel gets the cursor's x offset inside its row, in pixels. */
static inline int np_wrap_limit(int area_w) { return area_w - SB_W - FONT_CELL - 4; }
static inline int np_visible_rows(int area_h) { int r = (area_h - 2) / (FONT_CELL + 1); return r < 1 ? 1 : r; }
static int np_rows_ex(const notepad_t *np, int area_w, int *end_rel) {
    int limit = np_wrap_limit(area_w), rel = 0, row = 0;
    for (u32 i = 0; i < np->text_len; ) {
        int clen = ko_utf8_char_len((unsigned char)np->text_buf[i]);
        if (clen == 3 && i + 3 <= np->text_len) {
            if (rel > limit) { rel = 0; row++; }
            rel += FONT_CELL; i += 3;
            continue;
        }
        char c = np->text_buf[i];
        if (c == '\n' || rel > limit) { rel = 0; row++; if (c == '\n') { i++; continue; } }
        rel += FONT_CELL; i++;
    }
    if (end_rel) *end_rel = rel;
    return row + 1;
}
static inline int np_rows(const notepad_t *np, int area_w) { return np_rows_ex(np, area_w, 0); }

#endif
