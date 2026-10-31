/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SHELL_FS_H
#define SHELL_FS_H

/* shell_fs.h -- MiniShell's file commands: pwd cd ls mkdir touch cat cp mv rm -- Unix names first, with the Windows ones (dir md type copy move del) and MiniWin's own
 * (where go look dig new dup) as aliases; the dispatcher in terminal.h maps them. Deliberately NOT aliased to anything invented: the
 * two that can destroy data (rm) or are easy to mix up (mv), because a surprising name is the last thing you want next to a delete. They work on the namespace of vfs.h:
 * "/" is the root volume, "/mnt/<id>" any mounted volume, paths are absolute or relative to the current directory (which is
 * also what the prompt shows). Included by terminal.h, after its output helpers. */

/* After a command changed /docs or /progs: re-read the slot indexes and the desktop's own copy of them. */
static void term_fs_changed(void) {
    fs_rescan_docs();
    fs_rescan_progs();
    for (int i = 0; i < FS_MAX_FILES; i++) {
        u32 len = 0;
        desktop_file_exists[i] = fs_check_slot(i, &len) ? 1 : 0;
        desktop_file_len[i] = len;
    }
    desktop_refresh_programs();
}

static void term_fs_err(const char *what, int e) {
    char line[TERM_COLS + 1]; u32 l = 0;
    append_str(line, &l, sizeof(line), what);
    append_str(line, &l, sizeof(line), ": ");
    term_print(line);
    term_print_wrapped(mwfs_err_text(e), 2);
}

/* The prompt: the current directory, then "~# " for root or "~$ " for a user -- "/docs~# ", "/mnt/ide200/photos~$ ". A long path keeps
 * its END ("...x/y/z/a/b/c~# "), which is the part that says where you are. While a password is being asked for, the prompt says what
 * is wanted instead. */
static void term_prompt_str(char *out, u32 cap) {
    u32 l = 0;
    if (term_pw_mode != TP_NONE) {
        append_str(out, &l, cap, term_pw_mode == TP_NEW ? "New root password: " : (term_pw_mode == TP_CONFIRM ? "Retype it: " : "Password: "));
        out[l] = 0;
        return;
    }
    const char *p = vfs_is_root_ok() ? vfs_cwd : "?";
    u32 n = 0; while (p[n]) n++;
    if (n > 24) { append_str(out, &l, cap, "..."); p += n - 21; }
    append_str(out, &l, cap, p);
    append_str(out, &l, cap, term_is_root ? "~# " : "~$ ");
    out[l] = 0;
}

static int term_fs_ready(void) {
    if (vfs_is_root_ok()) return 1;
    term_print("  no root filesystem is mounted");
    term_print_wrapped(mwfs_err_text(vfs_root_err), 2);
    return 0;
}

static void term_cmd_where(void) {
    if (!term_fs_ready()) return;
    char line[TERM_COLS + 1]; u32 l = 0;
    append_str(line, &l, sizeof(line), "  ");
    append_str(line, &l, sizeof(line), vfs_cwd);
    term_print(line);
}
static void term_cmd_go(const char *arg) {
    if (!term_fs_ready()) return;
    int e = vfs_chdir(arg[0] ? arg : "/");
    if (e != MWFS_OK) term_fs_err(arg, e);
}

static void term_cmd_look(const char *arg) {
    if (!term_fs_ready()) return;
    vfs_node_t dir;
    int e = vfs_stat(arg[0] ? arg : ".", &dir);
    if (e != MWFS_OK) { term_fs_err(arg[0] ? arg : ".", e); return; }
    if (dir.type != MWFS_T_DIR) {                                    /* ls on a file: just that file */
        char line[TERM_COLS + 1]; u32 l = 0;
        append_str(line, &l, sizeof(line), "  ");
        append_str(line, &l, sizeof(line), dir.leaf);
        append_str(line, &l, sizeof(line), "  ");
        append_uint(line, &l, sizeof(line), (int)vfs_size(&dir));
        term_print(line);
        return;
    }
    u32 count = 0;
    mwfs_dirent_t ent;
    for (u32 i = 0; vfs_ls_nth(&dir, i, &ent) == 1; i++) {
        char line[TERM_COLS + 1]; u32 l = 0;
        append_str(line, &l, sizeof(line), "  ");
        append_str(line, &l, sizeof(line), ent.name);
        if (ent.type == MWFS_T_DIR) append_str(line, &l, sizeof(line), "/");
        else {
            while (l < 26) line[l++] = ' ';
            mwfs_ino_t t; u32 sz = 0;
            if (mwfs_ino_get(&vfs_vols[dir.vol], ent.ino, &t)) sz = t.size;
            line[l] = 0;
            append_uint(line, &l, sizeof(line), (int)sz);
        }
        term_print(line);
        count++;
    }
    if (!count) term_print("  (empty)");
    else {
        char line[TERM_COLS + 1]; u32 l = 0;
        append_str(line, &l, sizeof(line), "  ");
        append_uint(line, &l, sizeof(line), (int)count);
        append_str(line, &l, sizeof(line), count == 1 ? " entry" : " entries");
        term_print(line);
    }
}

static void term_cmd_dig(const char *arg) {
    if (!term_fs_ready()) return;
    if (!arg[0]) { term_print("  usage: mkdir <path>"); return; }
    int e = vfs_mkdir(arg);
    if (e != MWFS_OK) term_fs_err(arg, e);
}
static void term_cmd_zap(const char *arg) {
    if (!term_fs_ready()) return;
    if (!arg[0]) { term_print("  usage: rm <file or empty dir>"); return; }
    int e = vfs_rm(arg);
    if (e != MWFS_OK) term_fs_err(arg, e);
    else term_fs_changed();
}
static void term_cmd_new(const char *arg) {
    if (!term_fs_ready()) return;
    if (!arg[0]) { term_print("  usage: touch <path>"); return; }
    vfs_node_t n;
    int e = vfs_lookup(arg, &n);
    if (e == MWFS_OK && n.exists) return;                            /* already there: nothing to do (there are no timestamps yet) */
    if (e == MWFS_OK) e = vfs_write(arg, (const u8 *)"", 0, 0);
    if (e != MWFS_OK) term_fs_err(arg, e);
    else term_fs_changed();
}
static void term_cmd_dup(const char *a, const char *b) {
    if (!term_fs_ready()) return;
    if (!a[0] || !b[0]) { term_print("  usage: cp <from> <to>"); return; }
    int e = vfs_cp(a, b);
    if (e != MWFS_OK) term_fs_err(a, e);
    else term_fs_changed();
}
static void term_cmd_ship(const char *a, const char *b) {
    if (!term_fs_ready()) return;
    if (!a[0] || !b[0]) { term_print("  usage: mv <from> <to>"); return; }
    int e = vfs_mv(a, b);
    if (e != MWFS_OK) term_fs_err(a, e);
    else term_fs_changed();
}

static char term_cat_buf[FS_MAX_FILE_BYTES + 1];
static void term_cmd_cat(const char *arg) {
    if (!term_fs_ready()) return;
    if (!arg[0]) { term_print("  usage: rd <file>"); return; }
    u32 len = 0;
    int e = vfs_read(arg, (u8 *)term_cat_buf, FS_MAX_FILE_BYTES, &len, 0);
    if (e == MWFS_E_TOOBIG) {
        char line[TERM_COLS + 1]; u32 l = 0;
        append_str(line, &l, sizeof(line), "  too big to show (");
        append_uint(line, &l, sizeof(line), (int)len);
        append_str(line, &l, sizeof(line), " bytes, max 4096)");
        term_print(line);
        return;
    }
    if (e != MWFS_OK) { term_fs_err(arg, e); return; }
    if (len == 0) { term_print("  (empty file)"); return; }
    u32 bad = 0;
    for (u32 i = 0; i < len; i++) { u8 c = (u8)term_cat_buf[i]; if (c < 9 || (c > 13 && c < 32) || c > 126) bad++; }
    if (bad * 8 > len) {                                             /* mostly not text */
        char line[TERM_COLS + 1]; u32 l = 0;
        append_str(line, &l, sizeof(line), "  binary file, ");
        append_uint(line, &l, sizeof(line), (int)len);
        append_str(line, &l, sizeof(line), " bytes");
        term_print(line);
        return;
    }
    char line[TERM_COLS + 1]; u32 l = 0;
    for (u32 i = 0; i <= len; i++) {
        char c = i < len ? term_cat_buf[i] : '\n';
        if (c == '\n' || l >= (u32)term_wrap_cols() - 2) {
            line[l] = 0;
            if (i < len || l) term_print(line);
            l = 0;
            if (c == '\n') continue;
        }
        if (c == '\r') continue;
        if (i < len) line[l++] = (c == '\t') ? ' ' : (c < 32 || c > 126) ? '.' : c;
    }
}
#endif
