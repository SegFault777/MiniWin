/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_APPS_MINIFILES_H
#define MW_APPS_MINIFILES_H

/* apps/minifiles.h -- MINIFILES.MWP, the file manager, laid out like the Windows 95 Explorer (1.1-alpha-10).
 *
 *   +--------------------------------------------------------------+
 *   | MINIFILES.MWP                                        _ [] X  |  title bar (with its icon)
 *   | File  Edit  View  Tools  Help                                |  menu bar (ui/menubar.h)
 *   | [^][+] [Copy][Cut][Paste] [x][~][PC]                         |  tool bar
 *   | All Folders      | Contents of '/docs'                       |
 *   | [-] MiniWin      | Name        Size    Type    Modified      |  left: the folder tree (+/- to open and close)
 *   |   [+] docs       | NEWDOC.TXT  12 B    Text    26-10-25      |  right: the folder's contents, in columns
 *   |   [+] progs      | ...                                       |
 *   | 4 object(s)        | 1.2 KB  | Free: 520 KB                  |  status bar
 *   +--------------------------------------------------------------+
 *
 * The tree and the list both read the REAL directories of the root filesystem (MWFS, via vfs.h) -- before alpha-10 this
 * window showed a flat list of "documents" and "programs" taken from the slot index. Folders can be made, renamed, copied (files),
 * moved and deleted here. The Disks view (View menu) is the old DISKS tab: the IDE disks and their partitions, and INSTALL, which
 * types `os-inst <id>` into MiniShell (the one place that asks for a confirmation and prints the progress).
 *
 * Two files are special because the rest of the OS is built around slots: /docs holds what Notepad and the desktop show, /progs what
 * Start menu and desktop run. Changing either folder from here goes through the slot API (fs_delete_slot, prog_delete_slot) or ends
 * with fx_resync_desktop() so the desktop icons never disagree with the disk.
 *
 * Window plumbing follows Terminal's pattern exactly (window.h / windows.h / taskbar.h / compose.h / input.h each know about
 * WIN_ID_FILES); this file holds the window's own state, geometry, drawing and click handling.
 * Requires: vfs.h, fs.h, storage.h, ui/icon16.h, menubar.h, scrollbar.h, Notepad's open_document_slot(), mwp.h, term_*. */

#define FX_TREE_MAX    128         /* folders shown in the tree at once (an opened folder inserts its sub-folders) */
#define FX_ROWS_MAX    192         /* entries listed from one folder (more are cut off, and the status bar says so) */
#define FX_NAME        32          /* MWFS_NAME_MAX + 1 */
#define FX_ROW_H       16          /* one 16x16 icon */
#define FX_HDR_H       16          /* pane headers and the column header row */
#define FX_TOOL_H      20
#define FX_SAVED_MAX   24          /* opened folders remembered across a tree rebuild */
#define SB_OWNER_FILES_TREE 15     /* the tree's scroll bar (the list's is registered under WIN_ID_FILES); window ids use 0..WIN_ID_COUNT-1 */

#define FK_DIR   0                 /* order matters: folders sort before files */
#define FK_FILE  1
#define FK_DISK  2                 /* Disks view only: a whole device ("ide100") */
#define FK_PART  3                 /* Disks view only: a partition ("ide102") */

typedef struct { char name[FX_NAME]; int parent; u8 depth, expanded, has_kids, mount; } fx_node_t;
typedef struct { char name[FX_NAME]; char note[12]; u32 size, stamp; u8 kind, icon, bad, dev; } fx_row_t;

static fx_node_t fx_tree[FX_TREE_MAX];
static int  fx_tree_n = 0, fx_tree_sel = 0, fx_tscroll = 0;
static fx_row_t files_rows[FX_ROWS_MAX];
static int  files_row_count = 0, files_sel = -1, files_scroll = 0;
static char fx_cwd[VFS_PATH_MAX] = "/";
static int fx_cwd_is(const char *p);
/* The old DISKS view is gone (1.1-alpha-12): the disks and their partitions are what /mnt shows now, so "Disks view" simply means
 * "the current folder is /mnt". Mounted partitions inside it open as folders; the others are mounted from the tool bar. */
#define fx_view_disks (fx_cwd_is("/mnt"))
static int  fx_sort_key = 0, fx_sort_desc = 0;       /* 0 name, 1 size, 2 type, 3 modified */
static int  fx_armed = -1;                           /* row whose Delete was clicked once */
static int  fx_truncated = 0;                        /* the folder has more entries than FX_ROWS_MAX */
static u32  fx_ndirs = 0, fx_nfiles = 0, fx_bytes = 0;
static char files_msg[56];
static int  fx_btn_pressed = -1;
static int  fx_last_row = -1, fx_last_node = -1;
static u32  fx_last_tick = 0;
static char fx_clip[VFS_PATH_MAX];                   /* Copy / Cut: the path of the file; "" = nothing */
static int  fx_clip_cut = 0;
static int  fx_edit = 0, fx_edit_len = 0;            /* an inline rename is open on the selected row */
static char fx_edit_buf[FX_NAME];
static char fx_saved[FX_SAVED_MAX][VFS_PATH_MAX];
static u32  fx_free_kb_cache[VFS_MAX_VOLS];
static u8   fx_free_ok[VFS_MAX_VOLS];
static int  fx_started = 0;

static void files_set_msg(const char *s) { kstrcpy(files_msg, s, sizeof(files_msg)); }

/* ---------------------------------------------------------------- small helpers */
static inline char fx_up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static int fx_namecmp(const char *a, const char *b) {
    while (*a && fx_up(*a) == fx_up(*b)) { a++; b++; }
    return (int)(unsigned char)fx_up(*a) - (int)(unsigned char)fx_up(*b);
}
/* extension of a name, upper-cased, "" if none */
static void fx_ext(const char *name, char *out, int cap) {
    int dot = -1, n = 0;
    while (name[n]) { if (name[n] == '.') dot = n; n++; }
    int k = 0;
    if (dot >= 0) for (int i = dot + 1; name[i] && k < cap - 1; i++) out[k++] = fx_up(name[i]);
    out[k] = 0;
}
static int fx_ext_is(const char *name, const char *want) { char e[8]; fx_ext(name, e, sizeof e); return fx_namecmp(e, want) == 0; }

static int fx_drive_icon(const char *id) {
    if (id[0] == 'i' && id[1] == 'd' && id[2] == 'e') return IC_IDE;
    if (id[0] == 's' && id[1] == 'a') return IC_SATA;
    if (id[0] == 'n' && id[1] == 'v') return IC_NVME;
    if (id[0] == 's' && id[1] == 'c') return IC_SCSI;
    if (id[0] == 'u' && id[1] == 's') return IC_USB;
    if (id[0] == 'f' && id[1] == 'd') return IC_FLOPPY;
    if (id[0] == 'c' && id[1] == 'd') return IC_CD;
    return IC_DRIVE;
}
static int fx_file_icon(const char *name) {
    char e[8]; fx_ext(name, e, sizeof e);
    if (!fx_namecmp(e, "MWP")) return IC_PROG;
    if (!fx_namecmp(e, "HTML") || !fx_namecmp(e, "HTM")) return IC_HTML;
    if (!fx_namecmp(e, "CSS")) return IC_CSS;
    if (!fx_namecmp(e, "JS")) return IC_JS;
    if (!fx_namecmp(e, "C") || !fx_namecmp(e, "H")) return IC_C;
    if (!fx_namecmp(e, "ASM")) return IC_ASM;
    return IC_DOC;
}
static const char *fx_type_text(const fx_row_t *r) {
    if (r->kind == FK_DIR) return "Folder";
    if (r->kind == FK_DISK) return "Disk";
    if (r->kind == FK_PART) return "Part";
    char e[8]; fx_ext(r->name, e, sizeof e);
    if (!fx_namecmp(e, "MWP")) return "Prog";
    if (!fx_namecmp(e, "TXT")) return "Text";
    if (!fx_namecmp(e, "HTML") || !fx_namecmp(e, "HTM")) return "HTML";
    if (!fx_namecmp(e, "CSS")) return "CSS";
    if (!fx_namecmp(e, "JS")) return "Script";
    if (!fx_namecmp(e, "C") || !fx_namecmp(e, "H")) return "C src";
    if (!fx_namecmp(e, "ASM")) return "Asm";
    return "File";
}

/* "12 B", "3 KB", "1.5 MB" */
static void fx_fmt_bytes(char *out, u32 *l, u32 cap, u32 n) {
    if (n < 1024) { append_uint(out, l, cap, (int)n); append_str(out, l, cap, " B"); }
    else if (n < 1024u * 1024u) { append_uint(out, l, cap, (int)((n + 1023) / 1024)); append_str(out, l, cap, " KB"); }
    else { u32 t = (n / 1024) * 10 / 1024; append_uint(out, l, cap, (int)(t / 10)); append_str(out, l, cap, "."); append_uint(out, l, cap, (int)(t % 10)); append_str(out, l, cap, " MB"); }
}
/* "26-10-25" from an inode's packed stamp (see mwfs.h); "-" for none */
static void fx_fmt_date(char *out, u32 *l, u32 cap, u32 st) {
    if (!st) { append_str(out, l, cap, "-"); return; }
    append_uint2(out, l, cap, (int)(((st >> 25) + 1980) % 100)); append_str(out, l, cap, "-");
    append_uint2(out, l, cap, (int)((st >> 21) & 15)); append_str(out, l, cap, "-");
    append_uint2(out, l, cap, (int)((st >> 16) & 31));
}
static void fx_fmt_stamp(char *out, u32 *l, u32 cap, u32 st) {
    if (!st) { append_str(out, l, cap, "-"); return; }
    append_uint(out, l, cap, (int)((st >> 25) + 1980)); append_str(out, l, cap, "-");
    append_uint2(out, l, cap, (int)((st >> 21) & 15)); append_str(out, l, cap, "-");
    append_uint2(out, l, cap, (int)((st >> 16) & 31)); append_str(out, l, cap, " ");
    append_uint2(out, l, cap, (int)((st >> 11) & 31)); append_str(out, l, cap, ":");
    append_uint2(out, l, cap, (int)((st >> 5) & 63));
}
/* Draws s cut to max_w pixels (the font is fixed-width: FONT_CELL per character). */
static void fx_text_clip(int x, int y, const char *s, int max_w, u32 color) {
    char tmp[48]; int n = 0, cap = max_w / FONT_CELL;
    if (cap <= 0) return;
    while (s[n] && n < cap && n < (int)sizeof(tmp) - 1) { tmp[n] = s[n]; n++; }
    tmp[n] = 0;
    font_draw_string(x, y, tmp, color);
}

/* ---------------------------------------------------------------- the folder tree */
static void fx_node_path(int i, char *out) {
    int chain[20], n = 0;
    while (i > 0 && n < 20) { chain[n++] = i; i = fx_tree[i].parent; }
    u32 l = 0; out[0] = 0;
    if (n == 0) { out[0] = '/'; out[1] = 0; return; }
    while (n > 0) { kstrcpy_append(out, &l, VFS_PATH_MAX, '/'); append_str(out, &l, VFS_PATH_MAX, fx_tree[chain[--n]].name); }
}

typedef struct { int n; char names[48][FX_NAME]; } fx_kids_t;
static int fx_kids_cb(const mwfs_dirent_t *e, void *c) {
    fx_kids_t *k = (fx_kids_t *)c;
    if (e->type == MWFS_T_DIR && k->n < 48) kstrcpy(k->names[k->n++], e->name, FX_NAME);
    return 1;
}
static int fx_subdir_cb(const mwfs_dirent_t *e, void *c) { if (e->type == MWFS_T_DIR) { *(int *)c = 1; return 0; } return 1; }
static int fx_has_subdir(const char *path) {
    vfs_node_t n; int found = 0;
    if (vfs_stat(path, &n) != MWFS_OK || n.type != MWFS_T_DIR) return 0;
    vfs_ls_each(&n, fx_subdir_cb, &found);
    return found;
}

static void fx_tree_reset(void) {
    fx_tree_n = 1; fx_tree_sel = 0; fx_tscroll = 0;
    fx_tree[0].name[0] = 0; fx_tree[0].parent = -1; fx_tree[0].depth = 0; fx_tree[0].expanded = 0; fx_tree[0].mount = 0;
    fx_tree[0].has_kids = (u8)fx_has_subdir("/");
}

static void fx_expand(int i) {
    if (i < 0 || i >= fx_tree_n || fx_tree[i].expanded || !fx_tree[i].has_kids) return;
    char path[VFS_PATH_MAX]; fx_node_path(i, path);
    vfs_node_t dir; fx_kids_t kids; kids.n = 0;
    if (vfs_stat(path, &dir) != MWFS_OK) return;
    vfs_ls_each(&dir, fx_kids_cb, &kids);
    for (int a = 1; a < kids.n; a++) {                         /* alphabetical (insertion sort) */
        char tmp[FX_NAME]; kstrcpy(tmp, kids.names[a], FX_NAME);
        int b = a - 1;
        while (b >= 0 && fx_namecmp(kids.names[b], tmp) > 0) { kstrcpy(kids.names[b + 1], kids.names[b], FX_NAME); b--; }
        kstrcpy(kids.names[b + 1], tmp, FX_NAME);
    }
    int room = FX_TREE_MAX - fx_tree_n, cnt = kids.n < room ? kids.n : room;
    if (cnt <= 0) return;
    for (int j = fx_tree_n - 1; j > i; j--) fx_tree[j + cnt] = fx_tree[j];
    fx_tree_n += cnt;
    for (int j = i + 1 + cnt; j < fx_tree_n; j++) if (fx_tree[j].parent > i) fx_tree[j].parent += cnt;
    if (fx_tree_sel > i) fx_tree_sel += cnt;
    int is_mnt = (path[0] == '/' && path[1] == 'm' && path[2] == 'n' && path[3] == 't' && path[4] == 0);
    for (int k = 0; k < cnt; k++) {
        fx_node_t *n = &fx_tree[i + 1 + k];
        kstrcpy(n->name, kids.names[k], FX_NAME);
        n->parent = i; n->depth = (u8)(fx_tree[i].depth + 1); n->expanded = 0; n->mount = (u8)is_mnt;
    }
    fx_tree[i].expanded = 1;
    for (int k = 0; k < cnt; k++) {                            /* needs the nodes in place: fx_node_path walks parents */
        char cp[VFS_PATH_MAX]; fx_node_path(i + 1 + k, cp);
        fx_tree[i + 1 + k].has_kids = (u8)fx_has_subdir(cp);
    }
}
static void fx_collapse(int i) {
    if (i < 0 || i >= fx_tree_n || !fx_tree[i].expanded) return;
    int k = 0;
    while (i + 1 + k < fx_tree_n && fx_tree[i + 1 + k].depth > fx_tree[i].depth) k++;
    for (int j = i + 1 + k; j < fx_tree_n; j++) { fx_tree[j - k] = fx_tree[j]; if (fx_tree[j - k].parent > i + k) fx_tree[j - k].parent -= k; }
    fx_tree_n -= k;
    if (fx_tree_sel > i + k) fx_tree_sel -= k; else if (fx_tree_sel > i) fx_tree_sel = i;
    fx_tree[i].expanded = 0;
}
static int fx_find_child(int parent, const char *name) {
    for (int j = parent + 1; j < fx_tree_n && fx_tree[j].depth > fx_tree[parent].depth; j++)
        if (fx_tree[j].parent == parent && fx_namecmp(fx_tree[j].name, name) == 0) return j;
    return -1;
}
static int fx_tree_vis(void);
/* Walks `path` down the tree, opening the folders on the way (and the last one too when open_last). Returns the node reached. */
static int fx_tree_walk(const char *path, int open_last) {
    int cur = 0;
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) { if (open_last) fx_expand(0); return 0; }
    fx_expand(0);
    while (*p) {
        char tok[FX_NAME]; int l = 0;
        while (p[l] && p[l] != '/' && l < FX_NAME - 1) { tok[l] = p[l]; l++; }
        tok[l] = 0; p += l; while (*p == '/') p++;
        int c = fx_find_child(cur, tok);
        if (c < 0) { fx_expand(cur); c = fx_find_child(cur, tok); }
        if (c < 0) break;
        cur = c;
        if (*p || open_last) fx_expand(cur);
    }
    return cur;
}
static void fx_tree_scroll_to(int i) {
    int v = fx_tree_vis();
    if (i < fx_tscroll) fx_tscroll = i;
    if (i >= fx_tscroll + v) fx_tscroll = i - v + 1;
    if (fx_tscroll < 0) fx_tscroll = 0;
}

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

static inline int fx_tool_y(void)      { return files_win.y + TITLEBAR_H + MB_H + 2; }
static inline int fx_body_y(void)      { return fx_tool_y() + FX_TOOL_H + 2; }
static inline int files_status_y(void) { return files_win.y + files_win.h - 16; }
static inline int fx_body_h(void)      { return files_status_y() - 3 - fx_body_y(); }
static inline int fx_tree_x(void)      { return files_win.x + 4; }
static inline int fx_tree_w(void)      { int w = files_win.w * 3 / 10; return w < 110 ? 110 : (w > 170 ? 170 : w); }
static inline int fx_pane_x(void)      { return fx_tree_x() + fx_tree_w() + 4; }
static inline int fx_pane_w(void)      { return files_win.x + files_win.w - 4 - fx_pane_x(); }
/* the tree's list area, and the list's */
static inline int fx_tl_y(void) { return fx_body_y() + FX_HDR_H; }
static inline int fx_tl_h(void) { return fx_body_h() - FX_HDR_H; }
static inline int files_list_x(void) { return fx_pane_x(); }
static inline int files_list_y(void) { return fx_body_y() + 2 * FX_HDR_H; }
static inline int files_list_w(void) { return fx_pane_w(); }
static inline int files_list_h(void) { return fx_body_h() - 2 * FX_HDR_H; }
static int fx_tree_vis(void)         { int n = (fx_tl_h() - 4) / FX_ROW_H; return n < 1 ? 1 : n; }
static inline int files_visible_rows(void) { int n = (files_list_h() - 4) / FX_ROW_H; return n < 1 ? 1 : n; }
static int files_in_tree(int px, int py) { return in_rect(px, py, fx_tree_x(), fx_tl_y(), fx_tree_w(), fx_tl_h()); }

#define FX_SIZE_W 72
#define FX_TYPE_W 76
#define FX_MOD_W  104
#define FX_NAME_MIN 130
/* Which columns fit, and where they start. Narrow windows lose Type first, then Modified, then Size. */
typedef struct { int name_end, size_x, type_x, mod_x, show_size, show_type, show_mod, right; } fx_cols_t;
static void fx_cols(fx_cols_t *c) {
    int left = files_list_x() + 2, right = files_list_x() + files_list_w() - SB_W - 3, avail = right - left;
    c->right = right; c->show_size = c->show_type = c->show_mod = 0;
    if (avail >= FX_NAME_MIN + FX_SIZE_W + FX_MOD_W) c->show_size = c->show_mod = 1;
    else if (avail >= FX_NAME_MIN + FX_SIZE_W) c->show_size = 1;
    if (c->show_size && c->show_mod && avail >= FX_NAME_MIN + FX_SIZE_W + FX_MOD_W + FX_TYPE_W) c->show_type = 1;
    int x = right;
    if (c->show_mod)  { x -= FX_MOD_W;  c->mod_x = x; }
    if (c->show_type) { x -= FX_TYPE_W; c->type_x = x; }
    if (c->show_size) { x -= FX_SIZE_W; c->size_x = x; }
    c->name_end = x;
}

/* ---------------------------------------------------------------- the list */
static void fx_sort_rows(void) {
    for (int a = 1; a < files_row_count; a++) {
        fx_row_t t = files_rows[a];
        int b = a - 1;
        for (; b >= 0; b--) {
            const fx_row_t *o = &files_rows[b];
            int r;
            if (o->kind != t.kind) r = (int)o->kind - (int)t.kind;
            else {
                r = 0;
                if (fx_sort_key == 1) r = o->size < t.size ? -1 : (o->size > t.size ? 1 : 0);
                else if (fx_sort_key == 2) r = fx_namecmp(fx_type_text(o), fx_type_text(&t));
                else if (fx_sort_key == 3) r = o->stamp < t.stamp ? -1 : (o->stamp > t.stamp ? 1 : 0);
                if (r == 0) r = fx_namecmp(o->name, t.name);
                if (fx_sort_desc) r = -r;
            }
            if (r <= 0) break;
            files_rows[b + 1] = files_rows[b];
        }
        files_rows[b + 1] = t;
    }
}

static int fx_cwd_is(const char *p);
typedef struct { int in_mnt; int vol; int progs; } fx_ls_ctx_t;
static int fx_ls_cb(const mwfs_dirent_t *e, void *c) {
    fx_ls_ctx_t *x = (fx_ls_ctx_t *)c;
    if (files_row_count >= FX_ROWS_MAX) { fx_truncated = 1; return 0; }
    fx_row_t *r = &files_rows[files_row_count++];
    kstrcpy(r->name, e->name, FX_NAME);
    r->note[0] = 0; r->size = 0; r->stamp = 0; r->bad = 0; r->dev = 0;
    r->kind = (e->type == MWFS_T_DIR) ? FK_DIR : FK_FILE;
    mwfs_vol_t *v = x->in_mnt ? vfs_find_mount(e->name) : &vfs_vols[x->vol];
    mwfs_ino_t t;
    if (v && mwfs_ino_get(v, e->ino, &t)) { r->stamp = t.gen; if (r->kind == FK_FILE) r->size = t.size; }
    r->icon = (u8)(x->in_mnt ? fx_drive_icon(e->name) : (r->kind == FK_DIR ? IC_FOLDER : fx_file_icon(e->name)));
    if (!x->in_mnt && r->kind == FK_FILE && x->progs && fx_ext_is(e->name, "MWP")) {      /* in /progs a program shows the icon packed into its file, if it has one */
        int ps = prog_find_by_name(e->name);
        if (ps >= 0 && mwp_icon16_of(ps)) r->icon = (u8)(IC_CUSTOM_BASE + ps);
    }
    if (r->kind == FK_DIR) fx_ndirs++; else { fx_nfiles++; fx_bytes += r->size; }
    return 1;
}
static void fx_load_disks(void);
static void fx_load_dir(void) {
    if (fx_view_disks) { fx_load_disks(); return; }
    files_row_count = 0; fx_truncated = 0; fx_ndirs = fx_nfiles = fx_bytes = 0; fx_armed = -1;
    vfs_node_t d;
    if (vfs_stat(fx_cwd, &d) == MWFS_OK && d.type == MWFS_T_DIR) {
        fx_ls_ctx_t c; c.in_mnt = d.is_mnt_dir; c.vol = d.vol; c.progs = fx_cwd_is("/progs");
        vfs_ls_each(&d, fx_ls_cb, &c);
    }
    fx_sort_rows();
    if (files_sel >= files_row_count) files_sel = files_row_count - 1;
    if (files_scroll > 0 && files_scroll >= files_row_count) files_scroll = 0;
}
static void fx_load_disks(void) {
    files_row_count = 0; fx_truncated = 0; fx_armed = -1;
    st_scan();
    for (int d = 0; d < BLK_DEV_MAX && files_row_count < FX_ROWS_MAX - 10; d++) {
        if (!st_devs[d].present) continue;
        char id[16];
        st_type_t dtype = (st_type_t)blk_type_of(d);
        u32 dnum = (u32)(d - blk_first_of(dtype)) + 1;
        st_format_id(dtype, dnum, 0, id);
        int running_dev = (d == blk_boot_dev);
        fx_row_t *r = &files_rows[files_row_count++];
        kstrcpy(r->name, id, FX_NAME); kstrcpy(r->note, running_dev ? "RUNNING" : (vfs_find_mount(id) ? "MOUNTED" : ""), sizeof r->note);
        r->size = st_devs[d].sectors; r->stamp = 0; r->kind = FK_DISK; r->icon = (u8)fx_drive_icon(id); r->bad = (u8)running_dev; r->dev = (u8)d;
        st_part_t parts[8];
        int n = st_list_partitions(d, parts, 8);
        for (int i = 0; i < n && files_row_count < FX_ROWS_MAX; i++) {
            st_format_id(dtype, dnum, (u32)parts[i].number, id);
            const char *note = ""; int bad = 0;
            if (parts[i].extended_container) { note = "EXTENDED"; bad = 1; }
            else if (parts[i].sectors < ST_INSTALL_SECTORS) { note = "TOO SMALL"; bad = 1; }
            else if (d == blk_boot_dev && st_overlap(parts[i].start, parts[i].sectors, blk_boot_base, ST_INSTALL_SECTORS)) { note = "RUNNING"; bad = 1; }
            else if (vfs_find_mount(id)) note = "MOUNTED";
            fx_row_t *p = &files_rows[files_row_count++];
            kstrcpy(p->name, id, FX_NAME); kstrcpy(p->note, note, sizeof p->note);
            p->size = parts[i].sectors; p->stamp = 0; p->kind = FK_PART; p->icon = IC_DRIVE; p->bad = (u8)bad; p->dev = (u8)d;
        }
    }
    if (files_sel >= files_row_count) files_sel = files_row_count - 1;
    if (files_scroll > 0 && files_scroll >= files_row_count) files_scroll = 0;
}

static void fx_row_path(const fx_row_t *r, char *out) {
    u32 l = 0; out[0] = 0;
    append_str(out, &l, VFS_PATH_MAX, fx_cwd);
    if (!(fx_cwd[0] == '/' && fx_cwd[1] == 0)) kstrcpy_append(out, &l, VFS_PATH_MAX, '/');
    append_str(out, &l, VFS_PATH_MAX, r->name);
}
static int fx_cwd_is(const char *p) { return fx_namecmp(fx_cwd, p) == 0; }
/* the folders the system itself stands on */
static int fx_protected(const fx_row_t *r) {
    if (fx_cwd_is("/mnt")) return 1;
    if (fx_cwd_is("/") && (!fx_namecmp(r->name, "docs") || !fx_namecmp(r->name, "progs") || !fx_namecmp(r->name, "mnt") || !fx_namecmp(r->name, "sys"))) return 1;
    return 0;
}
static void fx_resync_desktop(void) {
    fs_rescan_docs(); fs_rescan_progs();
    for (int s = 0; s < FS_MAX_FILES; s++) { u32 l = 0; int ok = fs_check_slot(s, &l); desktop_file_exists[s] = (u8)(ok ? 1 : 0); desktop_file_len[s] = ok ? l : 0; }
    desktop_refresh_programs();
}
static void fx_free_invalidate(void) { for (int i = 0; i < VFS_MAX_VOLS; i++) fx_free_ok[i] = 0; }
/* Free space of a volume in KB: counts the clear bits of its bitmap (once, then cached until something changes). */
static u32 fx_free_kb(int vol) {
    if (vol < 0 || vol >= VFS_MAX_VOLS) return 0;
    if (fx_free_ok[vol]) return fx_free_kb_cache[vol];
    mwfs_vol_t *v = &vfs_vols[vol];
    u32 free_s = 0;
    for (u32 s = 0; s < v->bitmap_sectors; s++) {
        u8 b[512];
        if (!mwfs_rd(v, v->bitmap_lba + s, b)) break;
        for (u32 byte = 0; byte < 512; byte++) {
            for (u32 bit = 0; bit < 8; bit++) {
                u32 lba = s * 4096 + byte * 8 + bit;
                if (lba >= v->sectors) goto done;
                if (!((b[byte] >> bit) & 1)) free_s++;
            }
        }
    }
done:
    fx_free_kb_cache[vol] = free_s / 2; fx_free_ok[vol] = 1;
    return fx_free_kb_cache[vol];
}

/* Re-reads the tree (remembering which folders were open) and the list. Call after anything changed on disk. */
static void fx_refresh_all(void) {
    fx_free_invalidate();
    int saved = 0;
    for (int i = 0; i < fx_tree_n && saved < FX_SAVED_MAX; i++) if (fx_tree[i].expanded) fx_node_path(i, fx_saved[saved++]);
    fx_tree_reset();
    for (int k = 0; k < saved; k++) fx_tree_walk(fx_saved[k], 1);
    vfs_node_t n;
    if (vfs_stat(fx_cwd, &n) != MWFS_OK || n.type != MWFS_T_DIR) { fx_cwd[0] = '/'; fx_cwd[1] = 0; }   /* the folder we were in is gone */
    fx_tree_sel = fx_tree_walk(fx_cwd, 0);
    fx_tree_scroll_to(fx_tree_sel);
    fx_load_dir();
}
static void fx_goto(const char *path) {
    vfs_node_t n;
    char abs[VFS_PATH_MAX];
    if (vfs_norm(path, abs, sizeof abs) != MWFS_OK || vfs_stat(abs, &n) != MWFS_OK || n.type != MWFS_T_DIR) { files_set_msg("No such folder."); return; }
    kstrcpy(fx_cwd, abs, sizeof fx_cwd);
    fx_tree_sel = fx_tree_walk(fx_cwd, 0);
    fx_tree_scroll_to(fx_tree_sel);
    files_sel = -1; files_scroll = 0; fx_edit = 0;
    fx_load_dir();
    files_set_msg("");
}
static void fx_go_up(void) {
    if (fx_cwd[0] == '/' && fx_cwd[1] == 0) return;
    char p[VFS_PATH_MAX]; kstrcpy(p, fx_cwd, sizeof p);
    int n = 0; while (p[n]) n++;
    while (n > 1 && p[n - 1] != '/') n--;
    if (n > 1) n--;
    p[n] = 0; if (!p[0]) { p[0] = '/'; p[1] = 0; }
    fx_goto(p);
}
static void fx_select(int ri) {
    files_sel = ri; fx_armed = -1; fx_edit = 0; files_set_msg("");
    if (ri < 0 || ri >= files_row_count) return;
    fx_row_t *r = &files_rows[ri];
    if (!fx_view_disks && r->kind == FK_FILE && r->size <= 65536u && !r->bad) {      /* a quick integrity check of small files */
        char p[VFS_PATH_MAX]; vfs_node_t n; fx_row_path(r, p);
        if (vfs_stat(p, &n) == MWFS_OK && !mwfs_file_verify(&vfs_vols[n.vol], n.ino)) { r->bad = 1; kstrcpy(r->note, "DAMAGED", sizeof r->note); }
    }
    if (r->bad && r->note[0] == 'D') files_set_msg("That file is damaged.");
}
static void files_ensure_visible(void) {
    int v = files_visible_rows();
    if (files_sel < files_scroll) files_scroll = files_sel;
    if (files_sel >= files_scroll + v) files_scroll = files_sel - v + 1;
    if (files_scroll < 0) files_scroll = 0;
}
static int fx_select_name(const char *name) {
    for (int i = 0; i < files_row_count; i++) if (fx_namecmp(files_rows[i].name, name) == 0) { fx_select(i); files_ensure_visible(); return i; }
    return -1;
}

static void files_open_window(void) {
    files_win.open = 1; files_win.minimized = 0;
    if (!fx_started) { fx_started = 1; fx_tree_reset(); fx_expand(0); fx_cwd[0] = '/'; fx_cwd[1] = 0; fx_clip[0] = 0; fx_edit = 0; }   /* the root starts open, as in Explorer */
    fx_refresh_all();
    files_set_msg("");
    win_z_raise(WIN_ID_FILES);
}

/* ---------------------------------------------------------------- actions */
static void fx_type_into_shell(const char *cmd, const char *arg) {
    term_input_len = 0; term_input_buf[0] = 0;
    for (u32 i = 0; cmd[i]; i++) kstrcpy_append(term_input_buf, &term_input_len, sizeof(term_input_buf), cmd[i]);
    for (u32 i = 0; arg[i]; i++) kstrcpy_append(term_input_buf, &term_input_len, sizeof(term_input_buf), arg[i]);
    term_win.open = 1; term_win.minimized = 0;
    win_z_raise(WIN_ID_TERMINAL);
    term_input_focused = 1;
}

/* ---- /mnt actions (alpha-12): mount, unmount, install the selected disk or partition ---- */
static int fx_row_is_disk(int ri) { return ri >= 0 && ri < files_row_count && (files_rows[ri].kind == FK_DISK || files_rows[ri].kind == FK_PART); }
static int fx_row_mounted(int ri) { return fx_row_is_disk(ri) && vfs_find_mount(files_rows[ri].name) != 0; }

/* Mounts row `ri` under /mnt/<id> the way `part mount <id>` does, but says what happened on the status line instead of in the shell.
 * Returns 1 when it is mounted afterwards. (The rows are reloaded, so the row index is not valid after this: re-select by name.) */
static int fx_mount_row(int ri) {
    if (!fx_row_is_disk(ri)) { files_set_msg("Select a disk or a partition."); return 0; }
    char id[FX_NAME]; kstrcpy(id, files_rows[ri].name, FX_NAME);
    if (vfs_find_mount(id)) { files_set_msg("Already mounted."); return 1; }
    if (files_rows[ri].note[0] == 'E') { files_set_msg("Pick a logical partition inside it."); return 0; }
    st_target_t t;
    int e = part_resolve(id, &t, 0);
    if (e != ST_OK) { files_set_msg(st_error_text(e)); return 0; }
    u32 vb, vn;
    if (part_find_volume(&t, &vb, &vn) == 0) { files_set_msg("No MiniWin filesystem there (part format)."); return 0; }
    e = vfs_mount_region(t.id, t.pos, vb, vn, 0);
    if (e != MWFS_OK) { files_set_msg(mwfs_err_text(e)); return 0; }
    fx_refresh_all(); fx_select_name(id);
    files_set_msg("Mounted.");
    return 1;
}
static void fx_unmount_row(int ri) {
    if (!fx_row_is_disk(ri)) { files_set_msg("Select a disk or a partition."); return; }
    char id[FX_NAME]; kstrcpy(id, files_rows[ri].name, FX_NAME);
    if (!vfs_find_mount(id)) { files_set_msg("It is not mounted."); return; }
    int e = vfs_unmount(id);
    if (e != MWFS_OK) { files_set_msg(mwfs_err_text(e)); return; }
    fx_refresh_all(); fx_select_name(id);
    files_set_msg("Unmounted.");
}
/* The INSTALL button: types `os-inst <id> ` into MiniShell (the YES confirmation lives there, so nothing is erased from here). */
static void fx_install_row(int ri) {
    if (!fx_row_is_disk(ri)) { files_set_msg("Select a disk or a partition."); return; }
    fx_row_t *r = &files_rows[ri];
    if (r->bad) {                                               /* say WHY, in a sentence */
        if (r->note[0] == 'R') files_set_msg("That is where MiniWin runs from.");
        else if (r->note[0] == 'E') files_set_msg("Pick a logical partition inside it.");
        else if (r->note[0] == 'T') files_set_msg("Too small: needs at least 1 MB.");
        else files_set_msg("Cannot install there.");
        return;
    }
    fx_type_into_shell("os-inst ", r->name);
    files_set_msg("Press Enter in MiniShell.");
}

/* Open a folder / run a program / open a document / (in /mnt) open a disk's volume */
static void fx_open_row(int ri) {
    if (ri < 0 || ri >= files_row_count) { files_set_msg("Select something first."); return; }
    fx_row_t *r = &files_rows[ri];
    char p[VFS_PATH_MAX];
    if (r->kind == FK_DIR) { fx_row_path(r, p); fx_goto(p); return; }
    if (r->kind == FK_DISK || r->kind == FK_PART) {              /* /mnt: open it like a folder, mounting it first if need be */
        char id[FX_NAME]; kstrcpy(id, r->name, FX_NAME);
        if (!vfs_find_mount(id) && !fx_mount_row(ri)) return;
        u32 k = 0; append_str(p, &k, sizeof p, "/mnt/"); append_str(p, &k, sizeof p, id);
        fx_goto(p);
        return;
    }
    if (r->bad) { files_set_msg("That file is damaged."); return; }
    if (fx_ext_is(r->name, "MWP")) {
        int slot = fx_cwd_is("/progs") ? prog_find_by_name(r->name) : -1;
        if (slot < 0) { files_set_msg("Programs run from /progs."); return; }
        if (!prog_verify_slot(slot)) { files_set_msg("That program is damaged."); return; }
        const char *why = pw_launch(slot, r->name);               /* in a window, like `run` */
        files_set_msg(why ? why : "Started in a window.");
        return;
    }
    int slot = fx_cwd_is("/docs") ? fs_find_by_name(r->name) : -1;
    if (slot < 0) { files_set_msg("Only files in /docs open in Notepad."); return; }
    open_document_slot(slot);                                    /* sets the desktop status line itself */
    files_set_msg("Opened in Notepad.");
}

static void fx_after_change(const char *select_name) {
    fx_resync_desktop();
    fx_refresh_all();
    if (select_name) fx_select_name(select_name);
}

static void fx_delete_action(void) {
    if (fx_view_disks) return;
    if (files_sel < 0 || files_sel >= files_row_count) { files_set_msg("Select a file first."); return; }
    fx_row_t *r = &files_rows[files_sel];
    if (fx_protected(r)) { files_set_msg("The system needs that folder."); return; }
    if (fx_armed != files_sel) {                                 /* first click: arm */
        fx_armed = files_sel;
        u32 l = 0; char m[56];
        append_str(m, &l, sizeof(m), "Click DELETE again: ");
        append_str(m, &l, sizeof(m), r->name);
        files_set_msg(m);
        return;
    }
    char p[VFS_PATH_MAX]; fx_row_path(r, p);
    int e = MWFS_OK, keep = files_sel;
    if (r->kind == FK_FILE && fx_cwd_is("/docs")) {
        int slot = fs_find_by_name(r->name);
        if (slot >= 0 && find_notepad_bound_to(slot) >= 0) { files_set_msg("It is open in Notepad."); fx_armed = -1; return; }
        if (slot >= 0) { if (!fs_delete_slot(slot)) e = MWFS_E_IO; } else e = vfs_rm(p);
    } else if (r->kind == FK_FILE && fx_cwd_is("/progs")) {
        int slot = prog_find_by_name(r->name);
        if (slot >= 0) { if (!prog_delete_slot(slot)) e = MWFS_E_IO; } else e = vfs_rm(p);
    } else e = vfs_rm(p);
    fx_after_change(0);
    files_sel = keep < files_row_count ? keep : files_row_count - 1;
    if (e == MWFS_OK) files_set_msg("Deleted."); else files_set_msg(mwfs_err_text(e));
}

static void fx_new_folder(void) {
    if (fx_view_disks) return;
    if (fx_cwd_is("/mnt")) { files_set_msg("Mount a partition there instead."); return; }
    char name[FX_NAME], p[VFS_PATH_MAX];
    for (int n = 1; n < 100; n++) {
        u32 l = 0; name[0] = 0;
        append_str(name, &l, sizeof name, "NewFolder");
        if (n > 1) append_uint(name, &l, sizeof name, n);
        u32 pl = 0; p[0] = 0;
        append_str(p, &pl, sizeof p, fx_cwd);
        if (!(fx_cwd[0] == '/' && fx_cwd[1] == 0)) kstrcpy_append(p, &pl, sizeof p, '/');
        append_str(p, &pl, sizeof p, name);
        vfs_node_t nd;
        if (vfs_stat(p, &nd) == MWFS_E_NOENT) break;
    }
    int e = vfs_mkdir(p);
    if (e != MWFS_OK) { files_set_msg(mwfs_err_text(e)); return; }
    fx_after_change(name);
    if (files_sel >= 0) {                                        /* like Windows: the new folder is ready to be named */
        fx_edit = 1; fx_edit_len = 0; fx_edit_buf[0] = 0;
        for (int i = 0; name[i] && fx_edit_len < FX_NAME - 1; i++) { fx_edit_buf[fx_edit_len++] = name[i]; fx_edit_buf[fx_edit_len] = 0; }
    }
}
static void fx_begin_rename(void) {
    if (fx_view_disks || files_sel < 0 || files_sel >= files_row_count) { files_set_msg("Select something first."); return; }
    fx_row_t *r = &files_rows[files_sel];
    if (fx_protected(r)) { files_set_msg("The system needs that folder."); return; }
    fx_edit = 1; fx_edit_len = 0; fx_edit_buf[0] = 0;
    for (int i = 0; r->name[i] && fx_edit_len < FX_NAME - 1; i++) { fx_edit_buf[fx_edit_len++] = r->name[i]; fx_edit_buf[fx_edit_len] = 0; }
}
static void fx_commit_rename(void) {
    if (!fx_edit) return;
    fx_edit = 0;
    if (files_sel < 0 || files_sel >= files_row_count) return;
    fx_row_t *r = &files_rows[files_sel];
    if (!fx_edit_len || !mwfs_name_ok(fx_edit_buf)) { files_set_msg("That is not a valid name."); return; }
    if (!mwfs_name_eq(fx_edit_buf, r->name) == 0 && fx_edit_buf[0] == r->name[0] && mwfs_name_eq(fx_edit_buf, r->name)) return;   /* unchanged */
    char from[VFS_PATH_MAX], to[VFS_PATH_MAX], newname[FX_NAME];
    kstrcpy(newname, fx_edit_buf, sizeof newname);
    fx_row_path(r, from);
    fx_row_t tmp; kstrcpy(tmp.name, newname, FX_NAME); fx_row_path(&tmp, to);
    if (r->kind == FK_FILE && fx_cwd_is("/docs")) { int slot = fs_find_by_name(r->name); if (slot >= 0 && find_notepad_bound_to(slot) >= 0) { files_set_msg("It is open in Notepad."); return; } }
    vfs_node_t nd;
    if (vfs_stat(to, &nd) == MWFS_OK) { files_set_msg("That name is already used."); return; }
    int e = vfs_mv(from, to);
    if (e != MWFS_OK) { files_set_msg(mwfs_err_text(e)); return; }
    fx_after_change(newname);
    files_set_msg("Renamed.");
}
static void fx_copy_action(int cut) {
    if (fx_view_disks || files_sel < 0 || files_sel >= files_row_count) { files_set_msg("Select a file first."); return; }
    fx_row_t *r = &files_rows[files_sel];
    if (cut && fx_protected(r)) { files_set_msg("The system needs that folder."); return; }
    if (!cut && r->kind == FK_DIR) { files_set_msg("Folders can be moved (Cut), not copied."); return; }
    fx_row_path(r, fx_clip); fx_clip_cut = cut;
    files_set_msg(cut ? "Cut. Open a folder and Paste." : "Copied. Open a folder and Paste.");
}
static void fx_paste_action(void) {
    if (fx_view_disks) return;
    if (!fx_clip[0]) { files_set_msg("Nothing to paste."); return; }
    if (fx_cwd_is("/mnt")) { files_set_msg("Cannot paste into /mnt."); return; }
    int n = 0, last = 0; while (fx_clip[n]) { if (fx_clip[n] == '/') last = n; n++; }
    fx_row_t tmp; kstrcpy(tmp.name, fx_clip + last + 1, FX_NAME);
    char to[VFS_PATH_MAX]; fx_row_path(&tmp, to);
    vfs_node_t nd;
    if (vfs_stat(to, &nd) == MWFS_OK) { files_set_msg("A file with that name is already here."); return; }
    int e = fx_clip_cut ? vfs_mv(fx_clip, to) : vfs_cp(fx_clip, to);
    if (e != MWFS_OK) { files_set_msg(mwfs_err_text(e)); return; }
    if (fx_clip_cut) fx_clip[0] = 0;
    fx_after_change(tmp.name);
    files_set_msg("Pasted.");
}
static void fx_set_view(int disks) {                           /* View > Disks is just "go to /mnt"; View > Folders goes back to the root */
    fx_goto(disks ? "/mnt" : "/");
}
static void fx_sort_by(int key) {
    if (fx_view_disks) return;
    if (fx_sort_key == key) fx_sort_desc = !fx_sort_desc; else { fx_sort_key = key; fx_sort_desc = 0; }
    char keep[FX_NAME]; keep[0] = 0;
    if (files_sel >= 0 && files_sel < files_row_count) kstrcpy(keep, files_rows[files_sel].name, FX_NAME);
    fx_sort_rows();
    files_sel = -1;
    if (keep[0]) fx_select_name(keep);
}

/* ---------------------------------------------------------------- tool bar */
enum { FB_UP, FB_NEW, FB_COPY, FB_CUT, FB_PASTE, FB_DELETE, FB_REFRESH, FB_VIEW, FB_MOUNT, FB_UNMOUNT, FB_INSTALL, FB_COUNT };
static const u8 fb_icon[FB_COUNT] = { IC_UP, IC_FOLDER, 255, 255, 255, IC_TRASH, IC_REFRESH, IC_PC, 255, 255, 255 };
static const mb_txt_t fb_text[FB_COUNT] = {
    { 0, 0 }, { 0, 0 }, { "Copy", "\xeb\xb3\xb5\xec\x82\xac" }, { "Cut", "\xec\x9e\x98\xeb\x9d\xbc\xeb\x82\xb4\xea\xb8\xb0" }, { "Paste", "\xeb\xb6\x99\xec\x97\xac\xeb\x84\xa3\xea\xb8\xb0" },
    { 0, 0 }, { 0, 0 }, { 0, 0 }, { "Mount", "\xeb\xa7\x88\xec\x9a\xb4\xed\x8a\xb8" }, { "Unmount", "\xeb\xa7\x88\xec\x9a\xb4\xed\x8a\xb8\x20\xed\x95\xb4\xec\xa0\x9c" }, { "Install", "\xec\x84\xa4\xec\xb9\x98" }
};
static int fx_btn_w(int b) { return fb_icon[b] != 255 ? 26 : ko_string_width(mb_str(&fb_text[b])) + 10; }
/* Which buttons the current folder wants: /mnt trades the file operations for Mount / Unmount / Install. */
static int fx_btn_wanted(int b) {
    if (b == FB_MOUNT || b == FB_UNMOUNT || b == FB_INSTALL) return fx_view_disks;
    if (fx_view_disks) return b == FB_UP || b == FB_REFRESH || b == FB_VIEW;
    return 1;
}
static int fx_btn_x(int b) {
    int x = files_win.x + 4;
    for (int k = 0; k < b; k++) { if (!fx_btn_wanted(k)) continue; x += fx_btn_w(k) + 2; if (k == FB_UP || k == FB_NEW || k == FB_PASTE || k == FB_REFRESH || k == FB_VIEW) x += 6; }
    return x;
}
/* A button that would stick out past the window's right edge is left out instead of being drawn into the desktop (every action is
 * also in the menus, so nothing becomes unreachable). */
static int fx_btn_shown(int b) { return fx_btn_wanted(b) && fx_btn_x(b) + fx_btn_w(b) <= files_win.x + files_win.w - 4; }
static int fx_btn_enabled(int b) {
    if (b == FB_UP) return !(fx_cwd[0] == '/' && fx_cwd[1] == 0);
    if (b == FB_COPY || b == FB_CUT || b == FB_DELETE) return files_sel >= 0 && files_sel < files_row_count;
    if (b == FB_PASTE) return fx_clip[0] != 0;
    if (b == FB_MOUNT) return fx_row_is_disk(files_sel) && !fx_row_mounted(files_sel) && files_rows[files_sel].note[0] != 'E';
    if (b == FB_UNMOUNT) return fx_row_mounted(files_sel);
    if (b == FB_INSTALL) return fx_row_is_disk(files_sel) && !files_rows[files_sel].bad;
    return 1;
}
static int fx_btn_hit(int px, int py) {
    for (int b = 0; b < FB_COUNT; b++) {
        if (!fx_btn_shown(b)) continue;
        if (in_rect(px, py, fx_btn_x(b), fx_tool_y(), fx_btn_w(b), FX_TOOL_H)) return b;
    }
    return -1;
}
static void fx_btn_fire(int b) {
    if (!fx_btn_enabled(b)) return;
    switch (b) {
        case FB_UP: fx_go_up(); break;
        case FB_NEW: fx_new_folder(); break;
        case FB_COPY: fx_copy_action(0); break;
        case FB_CUT: fx_copy_action(1); break;
        case FB_PASTE: fx_paste_action(); break;
        case FB_DELETE: fx_delete_action(); break;
        case FB_REFRESH: fx_refresh_all(); files_set_msg("Refreshed."); break;
        case FB_VIEW: fx_set_view(!fx_view_disks); break;
        case FB_MOUNT: fx_mount_row(files_sel); break;
        case FB_UNMOUNT: fx_unmount_row(files_sel); break;
        case FB_INSTALL: fx_install_row(files_sel); break;
    }
}

/* ---------------------------------------------------------------- menus */
#define FILES_MENU_N 5
static const mb_menu_t files_menus[FILES_MENU_N] = {
    { { "File", "\xed\x8c\x8c\xec\x9d\xbc" }, 6, { { "Open", "\xec\x97\xb4\xea\xb8\xb0" }, { "New Folder", "\xec\x83\x88\x20\xed\x8f\xb4\xeb\x8d\x94" }, { "Rename", "\xec\x9d\xb4\xeb\xa6\x84\x20\xeb\xb0\x94\xea\xbe\xb8\xea\xb8\xb0" }, { "Delete", "\xec\x82\xad\xec\xa0\x9c" }, { "Refresh", "\xec\x83\x88\xeb\xa1\x9c\x20\xea\xb3\xa0\xec\xb9\xa8" }, { "Close window", "\xec\xb0\xbd\x20\xeb\x8b\xab\xea\xb8\xb0" } } },
    { { "Edit", "\xed\x8e\xb8\xec\xa7\x91" }, 3, { { "Copy", "\xeb\xb3\xb5\xec\x82\xac" }, { "Cut", "\xec\x9e\x98\xeb\x9d\xbc\xeb\x82\xb4\xea\xb8\xb0" }, { "Paste", "\xeb\xb6\x99\xec\x97\xac\xeb\x84\xa3\xea\xb8\xb0" } } },
    { { "View", "\xeb\xb3\xb4\xea\xb8\xb0" }, 6, { { "Folders", "\xed\x8f\xb4\xeb\x8d\x94" }, { "Disks", "\xeb\x94\x94\xec\x8a\xa4\xed\x81\xac" }, { "By Name", "\xec\x9d\xb4\xeb\xa6\x84\xec\x88\x9c" }, { "By Size", "\xed\x81\xac\xea\xb8\xb0\xec\x88\x9c" }, { "By Type", "\xec\xa2\x85\xeb\xa5\x98\xec\x88\x9c" }, { "By Date", "\xeb\x82\xa0\xec\xa7\x9c\xec\x88\x9c" } } },
    { { "Tools", "\xeb\x8f\x84\xea\xb5\xac" }, 4, { { "Mount", "\xeb\xa7\x88\xec\x9a\xb4\xed\x8a\xb8" }, { "Unmount", "\xeb\xa7\x88\xec\x9a\xb4\xed\x8a\xb8\x20\xed\x95\xb4\xec\xa0\x9c" }, { "Install MiniWin", "\x4d\x69\x6e\x69\x57\x69\x6e\x20\xec\x84\xa4\xec\xb9\x98" }, { "MiniShell here", "\xec\x97\xac\xea\xb8\xb0\xec\x84\x9c\x20\x4d\x69\x6e\x69\x53\x68\x65\x6c\x6c" } } },
    { { "Help", "\xeb\x8f\x84\xec\x9b\x80\xeb\xa7\x90" }, 1, { { "About MiniFiles", "\x4d\x69\x6e\x69\x46\x69\x6c\x65\x73\x20\xec\xa0\x95\xeb\xb3\xb4" } } },
};
static void files_menu_action(int menu, int item) {
    if (menu == 0) {
        if (item == 0) fx_open_row(files_sel);
        else if (item == 1) fx_new_folder();
        else if (item == 2) fx_begin_rename();
        else if (item == 3) fx_delete_action();
        else if (item == 4) { fx_refresh_all(); files_set_msg("Refreshed."); }
        else { files_win.open = 0; win_z_remove(WIN_ID_FILES); }
    } else if (menu == 1) {
        if (item == 0) fx_copy_action(0); else if (item == 1) fx_copy_action(1); else fx_paste_action();
    } else if (menu == 2) {
        if (item == 0) fx_set_view(0); else if (item == 1) fx_set_view(1); else fx_sort_by(item - 2);
    } else if (menu == 3) {
        if (item == 0) fx_mount_row(files_sel);
        else if (item == 1) fx_unmount_row(files_sel);
        else if (item == 2) fx_install_row(files_sel);
        else { fx_type_into_shell("cd ", fx_cwd); files_set_msg("Press Enter in MiniShell."); }
    } else {
        files_set_msg("MiniFiles: folders, files and disks");
    }
}

/* ---------------------------------------------------------------- input */
/* A click inside the window's content (the caller has already handled the title-bar buttons, the menu bar, dragging and the
 * scroll bars). */
static void files_click(int mx_, int my_, u32 now, u32 dbl_window) {
    if (fx_edit) {                                               /* a click anywhere ends the rename (Windows does the same) */
        int ry = files_list_y() + 2 + (files_sel - files_scroll) * FX_ROW_H;
        fx_cols_t c; fx_cols(&c);
        if (!in_rect(mx_, my_, files_list_x() + 2, ry, c.name_end - files_list_x(), FX_ROW_H)) fx_commit_rename();
        else return;
    }
    int b = fx_btn_hit(mx_, my_);
    if (b >= 0) { fx_btn_pressed = b; return; }
    /* the tree */
    if (files_in_tree(mx_, my_)) {
        int ni = fx_tscroll + (my_ - fx_tl_y() - 2) / FX_ROW_H;
        if (my_ - fx_tl_y() < 2 || ni < 0 || ni >= fx_tree_n) return;
        int x0 = fx_tree_x() + 2 + fx_tree[ni].depth * 16;
        if (fx_tree[ni].has_kids && mx_ >= x0 && mx_ < x0 + 12) {                 /* the +/- box */
            if (fx_tree[ni].expanded) fx_collapse(ni); else fx_expand(ni);
            return;
        }
        char p[VFS_PATH_MAX]; fx_node_path(ni, p);
        if (ni == fx_last_node && (now - fx_last_tick) < dbl_window) {            /* double-click: open or close it */
            fx_last_node = -1;
            if (fx_tree[ni].expanded) fx_collapse(ni); else fx_expand(ni);
            return;
        }
        fx_last_node = ni; fx_last_tick = now;
        fx_goto(p);
        return;
    }
    /* column headers: sort */
    if (!fx_view_disks && in_rect(mx_, my_, files_list_x(), fx_body_y() + FX_HDR_H, files_list_w(), FX_HDR_H)) {
        fx_cols_t c; fx_cols(&c);
        if (c.show_mod && mx_ >= c.mod_x) fx_sort_by(3);
        else if (c.show_type && mx_ >= c.type_x) fx_sort_by(2);
        else if (c.show_size && mx_ >= c.size_x) fx_sort_by(1);
        else fx_sort_by(0);
        return;
    }
    /* the list */
    if (in_rect(mx_, my_, files_list_x() + 2, files_list_y() + 2, files_list_w() - 4, files_list_h() - 4)) {
        int r = files_scroll + (my_ - files_list_y() - 2) / FX_ROW_H;
        if (r >= 0 && r < files_row_count) {
            int was = (r == files_sel);
            if (r == fx_last_row && (now - fx_last_tick) < dbl_window) { fx_last_row = -1; fx_select(r); fx_open_row(r); return; }
            fx_last_row = r; fx_last_tick = now;
            if (!was) fx_select(r);
            return;
        }
        fx_select(-1);
        return;
    }
}

/* Mouse released: fire the tool bar button that was pressed if the cursor is still over it. */
static void files_release(int mx_, int my_) {
    int b = fx_btn_pressed;
    fx_btn_pressed = -1;
    if (b < 0 || fx_btn_hit(mx_, my_) != b) return;
    fx_btn_fire(b);
}

/* Keyboard (the window has focus): arrows move, Enter opens, Backspace / Left goes up; while renaming, typing edits the name. */
static void files_key(int k) {
    if (fx_edit) {
        if (k == '\n') fx_commit_rename();
        else if (k == 27) fx_edit = 0;
        else if (k == '\b') { if (fx_edit_len > 0) fx_edit_buf[--fx_edit_len] = 0; }
        else if (k >= 32 && k < 127 && fx_edit_len < FX_NAME - 1) { fx_edit_buf[fx_edit_len++] = (char)k; fx_edit_buf[fx_edit_len] = 0; }
        return;
    }
    if (k == KEY_UP) { if (files_sel > 0) fx_select(files_sel - 1); else if (files_sel < 0 && files_row_count) fx_select(0); files_ensure_visible(); }
    else if (k == KEY_DOWN) { if (files_sel < files_row_count - 1) fx_select(files_sel + 1); files_ensure_visible(); }
    else if (k == KEY_LEFT || k == '\b') fx_go_up();
    else if (k == KEY_RIGHT || k == '\n') fx_open_row(files_sel);
    else if (k == 27 && !fx_view_disks) { fx_armed = -1; files_set_msg(""); }
}

/* ---------------------------------------------------------------- drawing */
static void fx_draw_hdr(int x, int y, int w, const char *text, int sort_mark) {
    ui_bevel(x, y, w, FX_HDR_H, 0);
    fx_text_clip(x + 4, y + 2, text, w - 8 - (sort_mark ? 10 : 0), TH_TEXT);
    if (sort_mark) {                                             /* a small triangle: up = ascending */
        int cx = x + w - 9, cy = y + 6;
        for (int i = 0; i < 3; i++) { int hw = fx_sort_desc ? 2 - i : i; for (int k = -hw; k <= hw; k++) bb_putpixel(cx + k, cy + i, TH_TEXT); }
    }
}
static void fx_draw_box(int x, int y, int open) {                 /* the 9x9 +/- box of a tree row */
    bb_fillrect(x, y, 9, 9, TH_FIELD); bb_rect(x, y, 9, 9, TH_SHADOW);
    for (int i = 2; i < 7; i++) bb_putpixel(x + i, y + 4, TH_FIELD_TEXT);
    if (!open) for (int j = 2; j < 7; j++) bb_putpixel(x + 4, y + j, TH_FIELD_TEXT);
}

static void draw_files_window(void) {
    int wx = files_win.x, wy = files_win.y, ww = files_win.w, wh = files_win.h;
    ui_window_frame_ic(wx, wy, ww, wh, files_win.maximized, "MINIFILES.MWP", IC_FILES);
    ui_titlebar_buttons(files_btn_min_x(), files_btn_max_x(), files_btn_close_x(), files_btn_y(),
                        pressed_btn_kind == BTN_MIN && pressed_btn_win == WIN_ID_FILES,
                        pressed_btn_kind == BTN_MAX && pressed_btn_win == WIN_ID_FILES,
                        pressed_btn_kind == BTN_CLOSE && pressed_btn_win == WIN_ID_FILES);
    mb_draw_bar(WIN_ID_FILES, wx, wy, ww, files_menus, FILES_MENU_N);
    if (files_scroll > 0 && files_scroll > files_row_count - files_visible_rows()) { files_scroll = files_row_count - files_visible_rows(); if (files_scroll < 0) files_scroll = 0; }
    if (fx_tscroll > 0 && fx_tscroll > fx_tree_n - fx_tree_vis()) { fx_tscroll = fx_tree_n - fx_tree_vis(); if (fx_tscroll < 0) fx_tscroll = 0; }

    /* ---- tool bar ---- */
    for (int b = 0; b < FB_COUNT; b++) {
        if (!fx_btn_shown(b)) continue;
        int x = fx_btn_x(b), y = fx_tool_y(), w = fx_btn_w(b), pr = (fx_btn_pressed == b), o = pr ? 1 : 0;
        ui_bevel(x, y, w, FX_TOOL_H, pr);
        if (fb_icon[b] != 255) ic16_blit(x + (w - 16) / 2 + o, y + (FX_TOOL_H - 16) / 2 + o, fb_icon[b]);
        else {
            const char *tx = mb_str(&fb_text[b]);
            ko_draw_mixed_string(x + (w - ko_string_width(tx)) / 2 + o, y + (FX_TOOL_H - FONT_CELL) / 2 + o, tx, fx_btn_enabled(b) ? TH_TEXT : TH_SHADOW);
        }
        if (!fx_btn_enabled(b) && fb_icon[b] != 255) {           /* a disabled icon button: dither it over with the face colour */
            for (int j = 0; j < 16; j++) for (int i = (j & 1); i < 16; i += 2) bb_putpixel(x + (w - 16) / 2 + i, y + (FX_TOOL_H - 16) / 2 + j, TH_FACE);
        }
    }

    /* ---- the folder tree (shown in /mnt too: the mounted volumes are its children) ---- */
    {
        int tx = fx_tree_x(), tw = fx_tree_w(), ty = fx_tl_y(), th = fx_tl_h();
        fx_draw_hdr(tx, fx_body_y(), tw, mb_str(&(mb_txt_t){ "All Folders", "\xeb\xaa\xa8\xeb\x93\xa0\x20\xed\x8f\xb4\xeb\x8d\x94" }), 0);
        ui_field(tx, ty, tw, th, TH_FIELD, 0);
        int vis = fx_tree_vis();
        for (int i = 0; i < vis && fx_tscroll + i < fx_tree_n; i++) {
            int ni = fx_tscroll + i;
            fx_node_t *n = &fx_tree[ni];
            int ry = ty + 2 + i * FX_ROW_H, x0 = tx + 3 + n->depth * 16;
            if (n->has_kids) fx_draw_box(x0 + 1, ry + 3, n->expanded);
            int ic = ni == 0 ? IC_PC : (n->mount ? fx_drive_icon(n->name) : (ni == fx_tree_sel ? IC_FOLDER_OPEN : IC_FOLDER));
            ic16_blit(x0 + 13, ry, ic);
            const char *label = ni == 0 ? "MiniWin" : n->name;
            int lx = x0 + 13 + 19, room = tx + tw - SB_W - 3 - lx;
            int lw = 0; while (label[lw]) lw++;
            lw *= FONT_CELL; if (lw > room) lw = room;
            if (room > 0) {
                if (ni == fx_tree_sel) bb_fillrect(lx - 2, ry + 1, lw + 4, FX_ROW_H - 2, TH_ACCENT);
                fx_text_clip(lx, ry + (FX_ROW_H - FONT_CELL) / 2, label, room, ni == fx_tree_sel ? TH_TEXT_INVERSE : TH_FIELD_TEXT);
            }
        }
        if (fx_tree_n > vis) sb_draw(SB_OWNER_FILES_TREE);
    }

    /* ---- the contents ---- */
    int px = fx_pane_x(), pw = fx_pane_w(), by = fx_body_y();
    char head[VFS_PATH_MAX + 16]; u32 hl = 0;
    if (fx_view_disks) append_str(head, &hl, sizeof head, mb_str(&(mb_txt_t){ "Disks and partitions", "\xeb\x94\x94\xec\x8a\xa4\xed\x81\xac\xec\x99\x80\x20\xed\x8c\x8c\xed\x8b\xb0\xec\x85\x98" }));
    else { append_str(head, &hl, sizeof head, mb_str(&(mb_txt_t){ "Contents of ", "\xeb\x82\xb4\xec\x9a\xa9\x3a\x20" })); append_str(head, &hl, sizeof head, fx_cwd); }
    fx_draw_hdr(px, by, pw, head, 0);
    fx_cols_t c; fx_cols(&c);
    {   /* column headers */
        int lx = files_list_x(), y2 = by + FX_HDR_H;
        fx_draw_hdr(lx, y2, c.name_end - lx, mb_str(&(mb_txt_t){ "Name", "\xec\x9d\xb4\xeb\xa6\x84" }), !fx_view_disks && fx_sort_key == 0);
        if (c.show_size) fx_draw_hdr(c.size_x, y2, FX_SIZE_W, mb_str(&(mb_txt_t){ "Size", "\xed\x81\xac\xea\xb8\xb0" }), !fx_view_disks && fx_sort_key == 1);
        if (c.show_type) fx_draw_hdr(c.type_x, y2, FX_TYPE_W, mb_str(&(mb_txt_t){ "Type", "\xec\xa2\x85\xeb\xa5\x98" }), !fx_view_disks && fx_sort_key == 2);
        if (c.show_mod) fx_draw_hdr(c.mod_x, y2, c.right + SB_W + 3 - c.mod_x, fx_view_disks ? "Note" : mb_str(&(mb_txt_t){ "Modified", "\xec\x88\x98\xec\xa0\x95\xed\x95\x9c\x20\xeb\x82\xa0\xec\xa7\x9c" }), !fx_view_disks && fx_sort_key == 3);
    }
    int lx = files_list_x(), ly = files_list_y(), lw = files_list_w(), lh = files_list_h();
    ui_field(lx, ly, lw, lh, TH_FIELD, 0);
    int vis = files_visible_rows();
    if (files_row_count == 0) font_draw_string(lx + 6, ly + 4, fx_view_disks ? "(no disk found)" : "(empty)", TH_FIELD_TEXT);
    for (int i = 0; i < vis && files_scroll + i < files_row_count; i++) {
        int ri = files_scroll + i;
        fx_row_t *r = &files_rows[ri];
        int ry = ly + 2 + i * FX_ROW_H, sel = (ri == files_sel);
        int ix = lx + 3 + (r->kind == FK_PART ? 16 : 0), ty = ry + (FX_ROW_H - FONT_CELL) / 2;
        ic16_blit(ix, ry, r->icon);
        int nx = ix + 19, nroom = c.name_end - 3 - nx;
        int nw = 0; while (r->name[nw]) nw++;
        nw *= FONT_CELL; if (nw > nroom) nw = nroom;
        if (fx_edit && sel) {                                    /* the inline rename box */
            ui_field(nx - 3, ry, nroom + 4 < 60 ? 60 : nroom + 4, FX_ROW_H, TH_FIELD, 1);
            fx_text_clip(nx, ty, fx_edit_buf, nroom, TH_FIELD_TEXT);
            int cx = nx + FONT_CELL * fx_edit_len;
            if (cx + 2 < nx + nroom) bb_fillrect(cx, ty, 2, FONT_CELL, TH_FIELD_TEXT);
        } else {
            u32 fg = TH_FIELD_TEXT;
            if (sel) { bb_fillrect(nx - 2, ry + 1, nw + 4, FX_ROW_H - 2, TH_ACCENT); fg = TH_TEXT_INVERSE; }
            fx_text_clip(nx, ty, r->name, nroom, fg);
        }
        char tmp[24]; u32 tl;
        if (c.show_size && (r->kind == FK_FILE || r->kind == FK_DISK || r->kind == FK_PART)) {
            tl = 0; tmp[0] = 0;
            if (r->kind == FK_FILE) fx_fmt_bytes(tmp, &tl, sizeof tmp, r->size);
            else { u32 mb = r->size / 2048; if (mb >= 10240) { append_uint(tmp, &tl, sizeof tmp, (int)(mb / 1024)); append_str(tmp, &tl, sizeof tmp, " GB"); } else { append_uint(tmp, &tl, sizeof tmp, (int)mb); append_str(tmp, &tl, sizeof tmp, " MB"); } }
            fx_text_clip(c.size_x + 4, ty, tmp, FX_SIZE_W - 6, TH_FIELD_TEXT);
        }
        if (c.show_type) fx_text_clip(c.type_x + 4, ty, fx_type_text(r), FX_TYPE_W - 6, TH_FIELD_TEXT);
        if (c.show_mod) {
            tl = 0; tmp[0] = 0;
            if (fx_view_disks) append_str(tmp, &tl, sizeof tmp, r->note); else fx_fmt_date(tmp, &tl, sizeof tmp, r->stamp);
            fx_text_clip(c.mod_x + 4, ty, tmp, FX_MOD_W - 6, (r->bad && !fx_view_disks) ? TH_ACCENT : TH_FIELD_TEXT);
        }
    }
    if (files_row_count > vis) sb_draw(WIN_ID_FILES);             /* the scroll bar (ui/scrollbar.h) */

    /* ---- status bar: [message or "N object(s)"] [size of the selection or of the folder] [free space] ---- */
    {
        int sy = files_status_y(), sx = wx + 4, sw = ww - 8;
        char a[64], bq[32], fq[32]; u32 al = 0, bl = 0, fl = 0; a[0] = bq[0] = fq[0] = 0;
        int sel_ok = files_sel >= 0 && files_sel < files_row_count;
        if (files_msg[0]) append_str(a, &al, sizeof a, files_msg);
        else if (sel_ok && !fx_view_disks && files_rows[files_sel].kind == FK_FILE) {
            fx_fmt_stamp(a, &al, sizeof a, files_rows[files_sel].stamp);
        } else {
            u32 cnt = fx_view_disks ? (u32)files_row_count : fx_ndirs + fx_nfiles;
            append_uint(a, &al, sizeof a, (int)cnt); append_str(a, &al, sizeof a, " object(s)");
            if (fx_truncated) append_str(a, &al, sizeof a, "+");
        }
        if (!fx_view_disks) {
            if (sel_ok && files_rows[files_sel].kind == FK_FILE) fx_fmt_bytes(bq, &bl, sizeof bq, files_rows[files_sel].size);
            else fx_fmt_bytes(bq, &bl, sizeof bq, fx_bytes);
            vfs_node_t cn;
            if (vfs_stat(fx_cwd, &cn) == MWFS_OK && !cn.is_mnt_dir) {
                u32 kb = fx_free_kb(cn.vol);
                append_str(fq, &fl, sizeof fq, "Free: ");
                if (kb >= 1024) { append_uint(fq, &fl, sizeof fq, (int)(kb / 1024)); append_str(fq, &fl, sizeof fq, " MB"); } else { append_uint(fq, &fl, sizeof fq, (int)kb); append_str(fq, &fl, sizeof fq, " KB"); }
            }
        }
        int wide = sw >= 420 && !files_msg[0];                      /* a message gets the whole bar: it is the thing to read */
        int fw = fq[0] && wide ? (int)fl * FONT_CELL + 10 : 0, mw = bq[0] && wide ? (int)bl * FONT_CELL + 10 : 0;
        int aw = sw - fw - mw - (fw ? 2 : 0) - (mw ? 2 : 0);
        ui_readout(sx, sy, aw, 14, 0); fx_text_clip(sx + 4, sy + 1, a, aw - 8, TH_TEXT);
        if (mw) { ui_readout(sx + aw + 2, sy, mw, 14, 0); font_draw_string(sx + aw + 2 + 5, sy + 1, bq, TH_TEXT); }
        if (fw) { ui_readout(sx + aw + 2 + mw + 2, sy, fw, 14, 0); font_draw_string(sx + aw + 2 + mw + 2 + 5, sy + 1, fq, TH_TEXT); }
    }
    mb_draw_drop(WIN_ID_FILES, wx, wy, files_menus, FILES_MENU_N);
}

#endif
