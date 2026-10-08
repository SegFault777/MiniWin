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
    const char *name = fs_slot_names[slot];
    while (*name && i < sizeof(status_buf) - 1) status_buf[i++] = *name++;
    status_buf[i] = 0;
    return status_buf;
}

/* Saves active_np->text_buf/active_np->text_len to the slot the current document is bound to,
 * or to the first empty slot if unbound (and binds to it, so subsequent
 * saves of the same still-open document update that slot instead of
 * creating a new file each time). Returns the slot saved to, or -1 if
 * all FS_MAX_FILES slots are already occupied ("disk full"). */
static int save_current_document(void) {
    int slot = active_np->bound_slot;
    if (slot < 0) {
        slot = fs_find_empty_slot();
        if (slot < 0) return -1;
    }
    if (!fs_save_slot(slot, active_np->text_buf, active_np->text_len)) return -1;
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

/* Shared Yes/No handling for the confirm dialog, used by both mouse
 * clicks and the Y/N keyboard shortcuts. What "Yes"/"No" actually do
 * depends on why the dialog was opened (active_np->confirm_mode). */
static void confirm_yes_action(void) {
    int saved_slot = save_current_document();
    if (active_np->confirm_mode == CONFIRM_NEW) {
        active_np->text_len = 0; active_np->text_buf[0] = 0; active_np->bound_slot = -1; ko_ime_reset();
        status = saved_slot >= 0 ? format_saved_status(saved_slot) : t(STR_STORAGE_FULL_NOT_SAVED);
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

#endif
