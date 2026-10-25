/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef VFS_H
#define VFS_H
#include "mwfs.h"

/* vfs.h -- the one namespace on top of the volumes: "/" is the root volume (the MWFS the running system lives on) and
 * "/mnt/<id>" is any other mounted volume (ide200, sata101, ...). Paths are absolute ("/docs/NOTE.TXT") or relative to the
 * terminal's current directory, may use "." and "..", and are case-insensitive like the DOS names they imitate.
 * Directories nest as deep as the path buffer allows (VFS_PATH_MAX characters).
 *
 * "/mnt" is a real directory on the root volume that stays empty; what `ls /mnt` shows are the mount points, which exist
 * only in the table below (they are gone after a reboot: mounting is a choice, not a setting). */

#define VFS_PATH_MAX 96
#define VFS_MAX_VOLS 6                                  /* the root volume + up to 5 mounted ones */

static mwfs_vol_t vfs_vols[VFS_MAX_VOLS];               /* [0] = "/" */
static int  vfs_root_err = MWFS_E_NOFS;                 /* MWFS_OK once the root volume is mounted */
static char vfs_cwd[VFS_PATH_MAX] = "/";
static u32  vfs_mnt_ino = 0;                            /* inode of the physical /mnt directory on the root volume */

static inline int vfs_is_root_ok(void) { return vfs_root_err == MWFS_OK; }

/* ---------------------------------------------------------------- paths */
/* Turns `in` (absolute, or relative to the current directory) into a canonical absolute path in out (no ".", "..", doubled
 * or trailing slashes). MWFS_E_NAME if a component is too long or the result does not fit. */
static inline int vfs_norm(const char *in, char *out, int outmax) {
    char tmp[VFS_PATH_MAX * 2 + 2];
    int n = 0;
    if (in[0] != '/') { for (const char *c = vfs_cwd; *c && n < (int)sizeof tmp - 2; c++) tmp[n++] = *c; if (n && tmp[n - 1] != '/') tmp[n++] = '/'; }
    for (const char *c = in; *c && n < (int)sizeof tmp - 1; c++) tmp[n++] = *c;
    tmp[n] = 0;
    int o = 0;
    out[o++] = '/';
    int comp_start[40], nc = 0;                         /* where each kept component starts in out */
    for (int i = 0; tmp[i]; ) {
        while (tmp[i] == '/') i++;
        if (!tmp[i]) break;
        int s = i;
        while (tmp[i] && tmp[i] != '/') i++;
        int len = i - s;
        if (len == 1 && tmp[s] == '.') continue;
        if (len == 2 && tmp[s] == '.' && tmp[s + 1] == '.') { if (nc) { o = comp_start[--nc]; } continue; }
        if (len > MWFS_NAME_MAX) return MWFS_E_NAME;
        if (nc >= 40) return MWFS_E_NAME;
        if (o + len + 2 > outmax) return MWFS_E_NAME;
        if (o > 1) out[o++] = '/';
        comp_start[nc++] = o > 1 && out[o - 1] == '/' ? o - 1 : o;
        for (int k = 0; k < len; k++) out[o++] = tmp[s + k];
    }
    if (o > 1 && out[o - 1] == '/') o--;
    out[o] = 0;
    return MWFS_OK;
}

static inline mwfs_vol_t *vfs_find_mount(const char *id) {
    for (int i = 1; i < VFS_MAX_VOLS; i++) if (vfs_vols[i].used && mwfs_name_eq(vfs_vols[i].id, id)) return &vfs_vols[i];
    return 0;
}

typedef struct {
    int  vol;                    /* index into vfs_vols[] */
    u32  ino;                    /* 0 if the path does not exist */
    u8   type;
    int  pvol; u32 pino;         /* the directory that holds (or would hold) it */
    char leaf[MWFS_NAME_MAX + 1];
    int  exists;
    int  is_mnt_dir;             /* the path is exactly /mnt */
    int  is_mountpoint;          /* the path is /mnt/<id> */
} vfs_node_t;

/* Resolves a CANONICAL absolute path. All components but the last must exist; the last may be missing (exists = 0, and
 * pvol/pino/leaf say where it would go -- what mkdir and file creation need). */
static inline int vfs_resolve(const char *abs, vfs_node_t *n) {
    mwfs_zero(n, sizeof *n);
    if (!vfs_is_root_ok()) return MWFS_E_NOFS;
    n->vol = 0; n->ino = MWFS_ROOT_INO; n->type = MWFS_T_DIR; n->pvol = 0; n->pino = 0; n->exists = 1;
    int depth = 0, prev_is_mnt = 0;
    const char *p = abs;
    while (*p == '/') p++;
    while (*p) {
        char tok[MWFS_NAME_MAX + 1];
        int len = 0;
        while (p[len] && p[len] != '/') { if (len >= MWFS_NAME_MAX) return MWFS_E_NAME; tok[len] = p[len]; len++; }
        tok[len] = 0;
        p += len;
        while (*p == '/') p++;
        int more = *p != 0;
        if (n->type != MWFS_T_DIR) return MWFS_E_NOTDIR;
        n->pvol = n->vol; n->pino = n->ino;
        mwfs_strcpy(n->leaf, tok, sizeof n->leaf);
        n->is_mnt_dir = 0; n->is_mountpoint = 0;
        if (n->vol == 0 && depth == 1 && prev_is_mnt) {            /* inside /mnt: names are mount points */
            mwfs_vol_t *m = vfs_find_mount(tok);
            if (m) { n->vol = (int)(m - vfs_vols); n->ino = MWFS_ROOT_INO; n->type = MWFS_T_DIR; n->exists = 1; n->is_mountpoint = 1; depth++; prev_is_mnt = 0; continue; }
        }
        u32 child; u8 ctype;
        int r = mwfs_dir_lookup(&vfs_vols[n->vol], n->ino, tok, &child, &ctype);
        if (r == MWFS_OK) { n->ino = child; n->type = ctype; n->exists = 1; }
        else if (r == MWFS_E_NOENT) { if (more) return MWFS_E_NOENT; n->ino = 0; n->type = 0; n->exists = 0; }
        else return r;
        prev_is_mnt = (n->vol == 0 && depth == 0 && mwfs_name_eq(tok, "mnt"));
        if (prev_is_mnt && n->exists) { n->is_mnt_dir = 1; if (!vfs_mnt_ino) vfs_mnt_ino = n->ino; }
        depth++;
    }
    return MWFS_OK;
}
static inline int vfs_lookup(const char *path, vfs_node_t *n) {
    char abs[VFS_PATH_MAX];
    int e = vfs_norm(path, abs, sizeof abs);
    return e != MWFS_OK ? e : vfs_resolve(abs, n);
}

/* ---------------------------------------------------------------- operations */
static inline int vfs_stat(const char *path, vfs_node_t *n) {
    int e = vfs_lookup(path, n);
    if (e != MWFS_OK) return e;
    return n->exists ? MWFS_OK : MWFS_E_NOENT;
}
static inline u32 vfs_size(const vfs_node_t *n) {
    mwfs_ino_t t;
    return (n->ino && mwfs_ino_get(&vfs_vols[n->vol], n->ino, &t)) ? t.size : 0;
}

/* The idx-th entry of a directory: for /mnt, the mount table; otherwise the directory itself. 1 = filled in, 0 = end, <0 = error. */
static inline int vfs_ls_nth(const vfs_node_t *dir, u32 idx, mwfs_dirent_t *out) {
    if (dir->is_mnt_dir) {
        u32 seen = 0;
        for (int i = 1; i < VFS_MAX_VOLS; i++) {
            if (!vfs_vols[i].used) continue;
            if (seen++ == idx) { out->ino = MWFS_ROOT_INO; out->type = MWFS_T_DIR; mwfs_strcpy(out->name, vfs_vols[i].id, sizeof out->name); return 1; }
        }
        return 0;
    }
    return mwfs_dir_nth(&vfs_vols[dir->vol], dir->ino, idx, out);
}

static inline int vfs_mkdir(const char *path) {
    vfs_node_t n;
    int e = vfs_lookup(path, &n);
    if (e != MWFS_OK) return e;
    if (n.exists) return MWFS_E_EXIST;
    if (n.pvol == 0 && n.pino == vfs_mnt_ino && vfs_mnt_ino) return MWFS_E_PERM;     /* mount points are made by `part mount` */
    return mwfs_mkdir(&vfs_vols[n.pvol], n.pino, n.leaf, 0);
}
static inline int vfs_write(const char *path, const u8 *data, u32 len, u32 aux) {
    vfs_node_t n;
    int e = vfs_lookup(path, &n);
    if (e != MWFS_OK) return e;
    if (n.exists && n.type == MWFS_T_DIR) return MWFS_E_ISDIR;
    if (n.pvol == 0 && n.pino == vfs_mnt_ino && vfs_mnt_ino) return MWFS_E_PERM;
    return mwfs_file_put(&vfs_vols[n.pvol], n.pino, n.leaf, data, len, aux);
}
static inline int vfs_read(const char *path, u8 *dst, u32 max, u32 *len, u32 *aux) {
    vfs_node_t n;
    int e = vfs_stat(path, &n);
    if (e != MWFS_OK) return e;
    if (n.type != MWFS_T_FILE) return MWFS_E_ISDIR;
    if (aux) { mwfs_ino_t t; if (mwfs_ino_get(&vfs_vols[n.vol], n.ino, &t)) *aux = t.aux; }
    return mwfs_file_get(&vfs_vols[n.vol], n.ino, dst, max, len);
}
static inline int vfs_rm(const char *path) {
    vfs_node_t n;
    int e = vfs_stat(path, &n);
    if (e != MWFS_OK) return e;
    if (n.is_mnt_dir || n.is_mountpoint || n.ino == MWFS_ROOT_INO) return MWFS_E_PERM;     /* "/", "/mnt" and the mounts: part unmount is the way */
    return mwfs_unlink(&vfs_vols[n.pvol], n.pino, n.leaf);
}
static inline int vfs_cp(const char *from, const char *to) {
    vfs_node_t a, b;
    int e = vfs_stat(from, &a);
    if (e != MWFS_OK) return e;
    if (a.type != MWFS_T_FILE) return MWFS_E_ISDIR;
    e = vfs_lookup(to, &b);
    if (e != MWFS_OK) return e;
    if (b.exists && b.type == MWFS_T_DIR) {                       /* "cp a /dir" means /dir/a */
        char full[VFS_PATH_MAX], abs[VFS_PATH_MAX];
        e = vfs_norm(to, abs, sizeof abs);
        if (e != MWFS_OK) return e;
        int n = 0; for (const char *c = abs; *c && n < VFS_PATH_MAX - 2; c++) full[n++] = *c;
        if (n > 1) full[n++] = '/';
        for (const char *c = a.leaf; *c && n < VFS_PATH_MAX - 1; c++) full[n++] = *c;
        full[n] = 0;
        return vfs_cp(from, full);
    }
    if (b.pvol == 0 && b.pino == vfs_mnt_ino && vfs_mnt_ino) return MWFS_E_PERM;
    if (b.exists && b.vol == a.vol && b.ino == a.ino) return MWFS_E_EXIST;
    return mwfs_file_copy(&vfs_vols[a.vol], a.ino, &vfs_vols[b.pvol], b.pino, b.leaf);
}
static inline int vfs_mv(const char *from, const char *to) {
    vfs_node_t a, b;
    int e = vfs_stat(from, &a);
    if (e != MWFS_OK) return e;
    if (a.is_mnt_dir || a.is_mountpoint || a.ino == MWFS_ROOT_INO) return MWFS_E_PERM;
    e = vfs_lookup(to, &b);
    if (e != MWFS_OK) return e;
    char dest[VFS_PATH_MAX];
    if (b.exists && b.type == MWFS_T_DIR) {                       /* "mv a /dir" means /dir/a */
        e = vfs_norm(to, dest, sizeof dest);
        if (e != MWFS_OK) return e;
        int n = 0; while (dest[n]) n++;
        if (n > 1 && n < VFS_PATH_MAX - 2) dest[n++] = '/';
        for (const char *c = a.leaf; *c && n < VFS_PATH_MAX - 1; c++) dest[n++] = *c;
        dest[n] = 0;
        e = vfs_lookup(dest, &b);
        if (e != MWFS_OK) return e;
    }
    if (b.pvol == 0 && b.pino == vfs_mnt_ino && vfs_mnt_ino) return MWFS_E_PERM;
    if (b.pvol == a.pvol) return mwfs_rename(&vfs_vols[a.pvol], a.pino, a.leaf, b.pino, b.leaf);
    if (a.type != MWFS_T_FILE) return MWFS_E_XDEV;                  /* directories do not move between volumes */
    e = mwfs_file_copy(&vfs_vols[a.vol], a.ino, &vfs_vols[b.pvol], b.pino, b.leaf);
    if (e != MWFS_OK) return e;
    return mwfs_unlink(&vfs_vols[a.pvol], a.pino, a.leaf);
}

/* `cd`: the target must be an existing directory. Mount points and "/mnt" count. */
static inline int vfs_chdir(const char *path) {
    char abs[VFS_PATH_MAX];
    int e = vfs_norm(path, abs, sizeof abs);
    if (e != MWFS_OK) return e;
    vfs_node_t n;
    e = vfs_resolve(abs, &n);
    if (e != MWFS_OK) return e;
    if (!n.exists) return MWFS_E_NOENT;
    if (n.type != MWFS_T_DIR) return MWFS_E_NOTDIR;
    mwfs_strcpy(vfs_cwd, abs, sizeof vfs_cwd);
    return MWFS_OK;
}

/* ---------------------------------------------------------------- mounting */
/* Opens the MWFS at dev/base (region = how many sectors there are from base) and enters it in the table as `id`. */
static inline int vfs_mount_region(const char *id, int dev, u32 base, u32 region, int *slot_out) {
    if (!vfs_is_root_ok()) return MWFS_E_NOFS;
    if (vfs_find_mount(id)) return MWFS_E_EXIST;
    int slot = -1;
    for (int i = 1; i < VFS_MAX_VOLS; i++) if (!vfs_vols[i].used) { slot = i; break; }
    if (slot < 0) return MWFS_E_FULL;
    mwfs_vol_t v;
    mwfs_zero(&v, sizeof v);
    v.dev = dev; v.base = base; v.sectors = region;
    int e = mwfs_open(&v);
    if (e != MWFS_OK) return e;
    v.used = 1;
    mwfs_strcpy(v.id, id, sizeof v.id);
    vfs_vols[slot] = v;
    if (slot_out) *slot_out = slot;
    return MWFS_OK;
}
static inline int vfs_unmount(const char *id) {
    mwfs_vol_t *m = vfs_find_mount(id);
    if (!m) return MWFS_E_NOENT;
    char pre[VFS_PATH_MAX];                                      /* never leave the terminal standing inside a volume that vanishes */
    int n = 0; const char *mp = "/mnt/";
    for (; mp[n]; n++) pre[n] = mp[n];
    for (int k = 0; m->id[k] && n < VFS_PATH_MAX - 1; k++) pre[n++] = m->id[k];
    pre[n] = 0;
    int inside = 1;
    for (int k = 0; pre[k]; k++) { char a = pre[k], b = vfs_cwd[k]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32); if (b >= 'A' && b <= 'Z') b = (char)(b + 32); if (a != b) { inside = 0; break; } }
    if (inside && (vfs_cwd[n] == 0 || vfs_cwd[n] == '/')) mwfs_strcpy(vfs_cwd, "/", sizeof vfs_cwd);
    mwfs_zero(m, sizeof *m);
    return MWFS_OK;
}

/* Creates the standard directories of a root volume. */
static const char *const vfs_root_dirs[] = { "docs", "progs", "mnt", "sys" };
static inline int vfs_make_root_layout(mwfs_vol_t *v) {
    for (unsigned i = 0; i < sizeof vfs_root_dirs / sizeof *vfs_root_dirs; i++) {
        int e = mwfs_mkdir(v, MWFS_ROOT_INO, vfs_root_dirs[i], 0);
        if (e != MWFS_OK && e != MWFS_E_EXIST) return e;
    }
    return MWFS_OK;
}

/* Mounts the root volume found at dev/base (region sectors long). Missing standard directories are re-created. */
static inline int vfs_mount_root(int dev, u32 base, u32 region) {
    mwfs_zero(&vfs_vols[0], sizeof vfs_vols[0]);
    vfs_vols[0].dev = dev; vfs_vols[0].base = base; vfs_vols[0].sectors = region;
    int e = mwfs_open(&vfs_vols[0]);
    if (e != MWFS_OK) { vfs_root_err = e; vfs_vols[0].used = 0; return e; }
    vfs_vols[0].used = 1;
    mwfs_strcpy(vfs_vols[0].id, "root", sizeof vfs_vols[0].id);
    vfs_root_err = MWFS_OK;
    vfs_mnt_ino = 0;
    mwfs_strcpy(vfs_cwd, "/", sizeof vfs_cwd);
    vfs_make_root_layout(&vfs_vols[0]);
    u32 i; u8 t;
    if (mwfs_dir_lookup(&vfs_vols[0], MWFS_ROOT_INO, "mnt", &i, &t) == MWFS_OK) vfs_mnt_ino = i;
    return MWFS_OK;
}
#endif
