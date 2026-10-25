/* SPDX-License-Identifier: GPL-3.0-only */
/* host_mwfs_test.c -- kernel/mwfs.h on a RAM disk: mkfs/open, files from 0 bytes to past the double-indirect boundary,
 * directories of any depth, replace/rename/unlink, a full volume, a randomized run checked against a model, and a CRASH
 * sweep: every operation is cut off after k sector writes for every k, and the volume must come back with each file
 * either old or new, nothing referenced-but-unmarked, and mwfs_check(fix) leaving a clean volume.
 *   tools/test/run_host_mwfs.sh */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IO_H
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            i32;

#define DISK_SECTORS 70000u
static u8 *disk;
static long writes_left = -1;           /* -1 = unlimited; otherwise writes after this many are silently dropped (the power went) */
static long write_count;
static int blk_read(int d, u32 lba, void *buf) { (void)d; if (lba >= DISK_SECTORS) return 0; memcpy(buf, disk + (size_t)lba * 512, 512); return 1; }
static int blk_write(int d, u32 lba, const void *buf) {
    (void)d; if (lba >= DISK_SECTORS) return 0;
    write_count++;
    if (writes_left == 0) return 1;     /* acknowledged, never stored: the machine is off */
    if (writes_left > 0) writes_left--;
    memcpy(disk + (size_t)lba * 512, buf, 512);
    return 1;
}
#include "../../kernel/mwfs.h"
#include "../../kernel/vfs.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static u32 rng = 12345;
static u32 rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void fill(u8 *p, u32 n, u32 seed) { u32 s = seed * 2654435761u + 1; for (u32 i = 0; i < n; i++) { s = s * 1103515245u + 12345u; p[i] = (u8)(s >> 16); } }
static int same(mwfs_vol_t *v, u32 dino, const char *name, u32 len, u32 seed) {
    u32 ino; u8 t;
    if (mwfs_dir_lookup(v, dino, name, &ino, &t) != MWFS_OK || t != MWFS_T_FILE) return 0;
    u8 *buf = malloc(len + 1), *want = malloc(len + 1);
    u32 got = 0;
    int ok = mwfs_file_get(v, ino, buf, len + 1, &got) == MWFS_OK && got == len;
    fill(want, len, seed);
    ok = ok && memcmp(buf, want, len) == 0 && mwfs_file_verify(v, ino);
    free(buf); free(want);
    return ok;
}
static void fresh(mwfs_vol_t *v, u32 sectors) {
    memset(disk, 0, (size_t)DISK_SECTORS * 512);
    memset(v, 0, sizeof *v);
    v->used = 1; v->dev = 0; v->base = 100; v->sectors = sectors;
    int e = mwfs_mkfs(v, "TEST");
    CHECK(e == MWFS_OK);
    mwfs_vol_t o; memset(&o, 0, sizeof o); o.dev = 0; o.base = 100; o.sectors = sectors;
    CHECK(mwfs_open(&o) == MWFS_OK);
    *v = o;
}
static int clean(mwfs_vol_t *v, int verbose) {
    mwfs_report_t r;
    int e = mwfs_check(v, 0, &r);
    if (verbose || e != MWFS_OK || r.bad_entries || r.bad_blocks || r.unmarked || r.damaged_files || r.orphans || r.leaked)
        if (!(e == MWFS_OK && !r.bad_entries && !r.bad_blocks && !r.unmarked && !r.damaged_files && !r.orphans && !r.leaked))
            printf("  check: e=%d files=%u dirs=%u orphans=%u badent=%u badblk=%u unmarked=%u leaked=%u damaged=%u\n", e, r.files, r.dirs, r.orphans, r.bad_entries, r.bad_blocks, r.unmarked, r.leaked, r.damaged_files);
    return e == MWFS_OK && !r.bad_entries && !r.bad_blocks && !r.unmarked && !r.damaged_files && !r.orphans && !r.leaked;
}

static u32 root_dir_sectors(mwfs_vol_t *v) { mwfs_ino_t r; mwfs_ino_get(v, MWFS_ROOT_INO, &r); return r.size / 512; }
static void test_basic(void) {
    printf("basic\n");
    mwfs_vol_t v; fresh(&v, 4000);
    CHECK(clean(&v, 0));
    u32 free0 = mwfs_free_count(&v);
    static const u32 sizes[] = { 0, 1, 511, 512, 513, 4096, 4097, 8 * 512, 8 * 512 + 1, 136 * 512, 136 * 512 + 1, 300 * 512 + 7 };
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        char name[16]; snprintf(name, sizeof name, "F%u.TXT", i);
        u8 *d = malloc(sizes[i] + 1); fill(d, sizes[i], i + 1);
        CHECK(mwfs_file_put(&v, MWFS_ROOT_INO, name, d, sizes[i], 7) == MWFS_OK);
        free(d);
        CHECK(same(&v, MWFS_ROOT_INO, name, sizes[i], i + 1));
    }
    CHECK(clean(&v, 0));
    mwfs_vol_t o = v; o.cur_blk = 0;
    CHECK(mwfs_open(&o) == MWFS_OK);
    CHECK(same(&o, MWFS_ROOT_INO, "f11.txt", sizes[11], 12));                    /* names are case-insensitive */
    /* double-indirect: a file bigger than 8 + 128 sectors needs the second level */
    u32 big = (8 + 128 + 700) * 512;
    u8 *b = malloc(big); fill(b, big, 99);
    CHECK(mwfs_file_put(&v, MWFS_ROOT_INO, "BIG.BIN", b, big, 0) == MWFS_OK);
    free(b);
    CHECK(same(&v, MWFS_ROOT_INO, "BIG.BIN", big, 99));
    CHECK(clean(&v, 0));
    /* replace shrinks and frees */
    u8 s[10]; fill(s, 10, 5);
    CHECK(mwfs_file_put(&v, MWFS_ROOT_INO, "BIG.BIN", s, 10, 0) == MWFS_OK);
    CHECK(same(&v, MWFS_ROOT_INO, "BIG.BIN", 10, 5));
    CHECK(clean(&v, 0));
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) { char name[16]; snprintf(name, sizeof name, "F%u.TXT", i); CHECK(mwfs_unlink(&v, MWFS_ROOT_INO, name) == MWFS_OK); }
    CHECK(mwfs_unlink(&v, MWFS_ROOT_INO, "BIG.BIN") == MWFS_OK);
    CHECK(clean(&v, 0));
    CHECK(mwfs_free_count(&v) + root_dir_sectors(&v) == free0);                  /* everything came back, except the sectors the root directory itself grew by */
    /* names */
    CHECK(mwfs_file_put(&v, MWFS_ROOT_INO, ".hidden", s, 1, 0) == MWFS_E_NAME);
    CHECK(mwfs_file_put(&v, MWFS_ROOT_INO, "has space", s, 1, 0) == MWFS_E_NAME);
    CHECK(mwfs_file_put(&v, MWFS_ROOT_INO, "this_name_is_far_too_long_for_the_filesystem", s, 1, 0) == MWFS_E_NAME);
    CHECK(mwfs_file_put(&v, MWFS_ROOT_INO, "a/b", s, 1, 0) == MWFS_E_NAME);
}

static void test_dirs(void) {
    printf("directories\n");
    mwfs_vol_t v; fresh(&v, 20000);
    u32 d1, d2, d3;
    CHECK(mwfs_mkdir(&v, MWFS_ROOT_INO, "x", &d1) == MWFS_OK);
    CHECK(mwfs_mkdir(&v, d1, "y", &d2) == MWFS_OK);
    CHECK(mwfs_mkdir(&v, d2, "z", &d3) == MWFS_OK);
    CHECK(mwfs_mkdir(&v, MWFS_ROOT_INO, "X", 0) == MWFS_E_EXIST);
    u8 a[3] = { 1, 2, 3 };
    CHECK(mwfs_file_put(&v, d3, "deep.txt", a, 3, 0) == MWFS_OK);
    CHECK(mwfs_file_put(&v, MWFS_ROOT_INO, "x", a, 3, 0) == MWFS_E_ISDIR);
    CHECK(mwfs_unlink(&v, d1, "y") == MWFS_E_NOTEMPTY);
    CHECK(mwfs_rename(&v, d3, "deep.txt", MWFS_ROOT_INO, "top.txt") == MWFS_OK);
    CHECK(mwfs_dir_lookup(&v, d3, "deep.txt", 0, 0) == MWFS_E_NOENT);
    CHECK(mwfs_dir_lookup(&v, MWFS_ROOT_INO, "top.txt", 0, 0) == MWFS_OK);
    CHECK(mwfs_unlink(&v, d2, "z") == MWFS_OK);
    CHECK(mwfs_unlink(&v, d1, "y") == MWFS_OK);
    CHECK(mwfs_unlink(&v, MWFS_ROOT_INO, "x") == MWFS_OK);
    CHECK(clean(&v, 0));
    /* a directory with hundreds of entries (many sectors) */
    for (int i = 0; i < 300; i++) { char n[16]; snprintf(n, sizeof n, "E%d", i); CHECK(mwfs_file_put(&v, MWFS_ROOT_INO, n, a, 3, 0) == MWFS_OK); }
    u32 n = 0; mwfs_dirent_t e; while (mwfs_dir_nth(&v, MWFS_ROOT_INO, n, &e) == 1) n++;
    CHECK(n == 301);                                                             /* 300 + top.txt */
    for (int i = 0; i < 300; i += 2) { char nm[16]; snprintf(nm, sizeof nm, "E%d", i); CHECK(mwfs_unlink(&v, MWFS_ROOT_INO, nm) == MWFS_OK); }
    n = 0; while (mwfs_dir_nth(&v, MWFS_ROOT_INO, n, &e) == 1) n++;
    CHECK(n == 151);
    CHECK(clean(&v, 0));
}

static void test_full(void) {
    printf("full volume\n");
    mwfs_vol_t v; fresh(&v, 600);
    u32 free0 = mwfs_free_count(&v);
    int made = 0, e = MWFS_OK;
    u8 *d = malloc(20 * 512); fill(d, 20 * 512, 3);
    for (int i = 0; i < 100 && e == MWFS_OK; i++) { char n[16]; snprintf(n, sizeof n, "G%d", i); e = mwfs_file_put(&v, MWFS_ROOT_INO, n, d, 20 * 512, 0); if (e == MWFS_OK) made++; }
    CHECK(e == MWFS_E_FULL);
    CHECK(made > 10);
    CHECK(clean(&v, 0));                                                         /* a failed put left nothing behind */
    for (int i = 0; i < made; i++) { char n[16]; snprintf(n, sizeof n, "G%d", i); CHECK(mwfs_unlink(&v, MWFS_ROOT_INO, n) == MWFS_OK); }
    CHECK(mwfs_free_count(&v) + root_dir_sectors(&v) == free0);
    free(d);
}

/* ---- model-based random run ---- */
#define MAXN 120
typedef struct { int used, isdir, parent; char name[12]; u32 ino, len, seed; } node_t;
static node_t nodes[MAXN];
static void path_of(int i, char *out) { if (i == 0) { out[0] = 0; return; } char t[64]; path_of(nodes[i].parent, t); snprintf(out, 128, "%s/%s", t, nodes[i].name); }
static int find_child(int parent, const char *name) { for (int i = 1; i < MAXN; i++) if (nodes[i].used && nodes[i].parent == parent && !strcasecmp(nodes[i].name, name)) return i; return 0; }
static void verify_all(mwfs_vol_t *v) {
    for (int i = 1; i < MAXN; i++) {
        if (!nodes[i].used) continue;
        u32 pino = nodes[nodes[i].parent].ino;
        u32 ino; u8 t;
        CHECK(mwfs_dir_lookup(v, pino, nodes[i].name, &ino, &t) == MWFS_OK);
        if (nodes[i].isdir) CHECK(t == MWFS_T_DIR); else CHECK(same(v, pino, nodes[i].name, nodes[i].len, nodes[i].seed));
    }
}
static void test_random(void) {
    printf("random vs model\n");
    mwfs_vol_t v; fresh(&v, 12000);
    memset(nodes, 0, sizeof nodes);
    nodes[0].used = 1; nodes[0].isdir = 1; nodes[0].ino = MWFS_ROOT_INO;
    for (int step = 0; step < 4000; step++) {
        int op = rnd() % 10;
        int target = rnd() % MAXN;
        if (op < 3) {                                                            /* create/replace a file */
            int parent; do { parent = rnd() % MAXN; } while (!nodes[parent].used || !nodes[parent].isdir);
            char nm[12]; snprintf(nm, sizeof nm, "N%u", rnd() % 40);
            int ex = find_child(parent, nm);
            if (ex && nodes[ex].isdir) continue;
            u32 len = (rnd() % 5 == 0) ? rnd() % 90000 : rnd() % 3000, seed = rnd();
            u8 *d = malloc(len + 1); fill(d, len, seed);
            int e = mwfs_file_put(&v, nodes[parent].ino, nm, d, len, 0);
            free(d);
            CHECK(e == MWFS_OK || e == MWFS_E_FULL);
            if (e == MWFS_OK) {
                int slot = ex;
                if (!slot) { for (slot = 1; slot < MAXN && nodes[slot].used; slot++) ; if (slot == MAXN) { mwfs_unlink(&v, nodes[parent].ino, nm); continue; } }
                nodes[slot].used = 1; nodes[slot].isdir = 0; nodes[slot].parent = parent; strcpy(nodes[slot].name, nm); nodes[slot].len = len; nodes[slot].seed = seed;
                u32 ino; mwfs_dir_lookup(&v, nodes[parent].ino, nm, &ino, 0); nodes[slot].ino = ino;
            }
        } else if (op < 5) {                                                     /* mkdir */
            int parent; do { parent = rnd() % MAXN; } while (!nodes[parent].used || !nodes[parent].isdir);
            char nm[12]; snprintf(nm, sizeof nm, "D%u", rnd() % 20);
            if (find_child(parent, nm)) continue;
            int slot; for (slot = 1; slot < MAXN && nodes[slot].used; slot++) ;
            if (slot == MAXN) continue;
            u32 ino; int e = mwfs_mkdir(&v, nodes[parent].ino, nm, &ino);
            CHECK(e == MWFS_OK || e == MWFS_E_FULL);
            if (e == MWFS_OK) { nodes[slot].used = 1; nodes[slot].isdir = 1; nodes[slot].parent = parent; strcpy(nodes[slot].name, nm); nodes[slot].ino = ino; }
        } else if (op < 8) {                                                     /* unlink */
            if (target == 0 || !nodes[target].used) continue;
            int kids = 0; for (int i = 1; i < MAXN; i++) if (nodes[i].used && nodes[i].parent == target) kids++;
            int e = mwfs_unlink(&v, nodes[nodes[target].parent].ino, nodes[target].name);
            if (kids) CHECK(e == MWFS_E_NOTEMPTY); else { CHECK(e == MWFS_OK); nodes[target].used = 0; }
        } else {                                                                 /* rename a file within or across dirs */
            if (target == 0 || !nodes[target].used || nodes[target].isdir) continue;
            int parent; do { parent = rnd() % MAXN; } while (!nodes[parent].used || !nodes[parent].isdir);
            char nm[12]; snprintf(nm, sizeof nm, "R%u", rnd() % 30);
            int ex = find_child(parent, nm);
            if (ex && nodes[ex].isdir) continue;
            int e = mwfs_rename(&v, nodes[nodes[target].parent].ino, nodes[target].name, nodes[parent].ino, nm);
            CHECK(e == MWFS_OK || e == MWFS_E_FULL);
            if (e == MWFS_OK) { if (ex && ex != target) nodes[ex].used = 0; nodes[target].parent = parent; strcpy(nodes[target].name, nm); }
        }
        if (step % 500 == 0) { verify_all(&v); CHECK(clean(&v, 0)); }
    }
    verify_all(&v);
    CHECK(clean(&v, 0));
    mwfs_vol_t o = v; CHECK(mwfs_open(&o) == MWFS_OK); verify_all(&o);         /* and after a "reboot" */
}

/* ---- the crash sweep ---- */
static u8 *snap;
static int post_crash_ok(mwfs_vol_t *v0, const char *what, int k) {
    mwfs_vol_t v = *v0; v.sectors = v0->sectors;
    mwfs_vol_t o; memset(&o, 0, sizeof o); o.dev = 0; o.base = 100; o.sectors = v0->sectors;
    int e = mwfs_open(&o);
    if (e != MWFS_OK) { printf("  %s k=%d: reopen failed %d\n", what, k, e); return 0; }
    (void)v;
    mwfs_report_t r;
    if (mwfs_check(&o, 0, &r) != MWFS_OK || r.bad_entries || r.bad_blocks || r.unmarked || r.damaged_files) {
        printf("  %s k=%d: unsafe state badent=%u badblk=%u unmarked=%u damaged=%u\n", what, k, r.bad_entries, r.bad_blocks, r.unmarked, r.damaged_files); return 0;
    }
    if (mwfs_check(&o, 1, &r) != MWFS_OK) return 0;
    if (!clean(&o, 0)) { printf("  %s k=%d: not clean after fix\n", what, k); return 0; }
    return 1;
}
static void test_crash(void) {
    printf("crash sweep\n");
    mwfs_vol_t base; fresh(&base, 3000);
    u8 *oldd = malloc(30 * 512 + 5), *newd = malloc(50 * 512 + 9);
    fill(oldd, 30 * 512 + 5, 1); fill(newd, 50 * 512 + 9, 2);
    u32 sub;
    CHECK(mwfs_mkdir(&base, MWFS_ROOT_INO, "d", &sub) == MWFS_OK);
    CHECK(mwfs_file_put(&base, MWFS_ROOT_INO, "KEEP", oldd, 30 * 512 + 5, 0) == MWFS_OK);
    CHECK(mwfs_file_put(&base, sub, "VICTIM", oldd, 30 * 512 + 5, 0) == MWFS_OK);
    size_t sz = (size_t)DISK_SECTORS * 512;
    snap = malloc(sz); memcpy(snap, disk, sz);
    u32 free_base = mwfs_free_count(&base);
    static const char *ops[] = { "replace", "create", "unlink", "rename", "mkdir", "dirgrow" };
    for (int op = 0; op < 6; op++) {
        /* how many writes does the operation make when it completes? */
        memcpy(disk, snap, sz); mwfs_vol_t v = base; v.cur_blk = v.data_lba; v.cur_ino = 1; writes_left = -1; write_count = 0;
        long total = 0;
        for (int k = -1; ; k++) {
            memcpy(disk, snap, sz); v = base; v.cur_blk = v.data_lba; v.cur_ino = 1;
            write_count = 0; writes_left = (k < 0) ? -1 : k;
            switch (op) {
                case 0: mwfs_file_put(&v, sub, "VICTIM", newd, 50 * 512 + 9, 0); break;
                case 1: mwfs_file_put(&v, sub, "FRESH", newd, 50 * 512 + 9, 0); break;
                case 2: mwfs_unlink(&v, sub, "VICTIM"); break;
                case 3: mwfs_rename(&v, sub, "VICTIM", MWFS_ROOT_INO, "MOVED"); break;
                case 4: { u32 x; mwfs_mkdir(&v, sub, "NEWDIR", &x); } break;
                case 5: { char n[16]; for (int i = 0; i < 20; i++) { snprintf(n, sizeof n, "Q%d", i); mwfs_file_put(&v, sub, n, oldd, 10, 0); } } break;
            }
            if (k < 0) { total = write_count; writes_left = -1; if (!post_crash_ok(&base, ops[op], -1)) CHECK(0); else CHECK(1); continue; }
            writes_left = -1;
            int ok = post_crash_ok(&base, ops[op], k);
            CHECK(ok);
            /* the surviving files are old-or-new, never a mix; KEEP is untouched */
            mwfs_vol_t o; memset(&o, 0, sizeof o); o.dev = 0; o.base = 100; o.sectors = base.sectors;
            if (mwfs_open(&o) == MWFS_OK) {
                CHECK(same(&o, MWFS_ROOT_INO, "KEEP", 30 * 512 + 5, 1));
                if (op == 0) CHECK(same(&o, sub, "VICTIM", 30 * 512 + 5, 1) || same(&o, sub, "VICTIM", 50 * 512 + 9, 2));
                if (op == 2 || op == 3) { u32 i1 = 0, i2 = 0; int a = mwfs_dir_lookup(&o, sub, "VICTIM", &i1, 0) == MWFS_OK; int b = mwfs_dir_lookup(&o, MWFS_ROOT_INO, "MOVED", &i2, 0) == MWFS_OK;
                    CHECK(op == 2 ? 1 : (a || b));                              /* a rename never loses the file */
                    if (a) { CHECK(same(&o, sub, "VICTIM", 30 * 512 + 5, 1)); }
                    if (b) { CHECK(same(&o, MWFS_ROOT_INO, "MOVED", 30 * 512 + 5, 1)); } }
                if (op == 1) { u32 i1; if (mwfs_dir_lookup(&o, sub, "FRESH", &i1, 0) == MWFS_OK) CHECK(same(&o, sub, "FRESH", 50 * 512 + 9, 2)); }
                CHECK(mwfs_free_count(&o) <= free_base + 64 && mwfs_free_count(&o) + 200 >= free_base);
            }
            if (k >= total) break;
        }
        printf("  %-8s swept %ld cut points\n", ops[op], total);
    }
    free(oldd); free(newd); free(snap);
    writes_left = -1;
}


static int norm_is(const char *cwd, const char *in, const char *want) {
    mwfs_strcpy(vfs_cwd, cwd, sizeof vfs_cwd);
    char out[VFS_PATH_MAX];
    int e = vfs_norm(in, out, sizeof out);
    if (e != MWFS_OK || strcmp(out, want)) { printf("  norm(%s,%s) = %s (e=%d), want %s\n", cwd, in, out, e, want); return 0; }
    return 1;
}
static void test_vfs(void) {
    printf("vfs\n");
    CHECK(norm_is("/", "/", "/"));
    CHECK(norm_is("/", "", "/"));
    CHECK(norm_is("/a/b", "..", "/a"));
    CHECK(norm_is("/a/b", "../..", "/"));
    CHECK(norm_is("/a/b", "../../..", "/"));
    CHECK(norm_is("/a/b", "c/./d/", "/a/b/c/d"));
    CHECK(norm_is("/a/b", "/x//y/../z", "/x/z"));
    CHECK(norm_is("/a", "b/../../c", "/c"));
    CHECK(norm_is("/", "x/y/z/a/b/c", "/x/y/z/a/b/c"));
    char longname[64]; memset(longname, 'a', 40); longname[40] = 0; char out[VFS_PATH_MAX];
    CHECK(vfs_norm(longname, out, sizeof out) == MWFS_E_NAME);

    memset(disk, 0, (size_t)DISK_SECTORS * 512);
    memset(vfs_vols, 0, sizeof vfs_vols); vfs_root_err = MWFS_E_NOFS; vfs_mnt_ino = 0;
    mwfs_vol_t m; memset(&m, 0, sizeof m); m.dev = 0; m.base = 100; m.sectors = 20000;
    CHECK(mwfs_mkfs(&m, "ROOT") == MWFS_OK);
    mwfs_vol_t m2; memset(&m2, 0, sizeof m2); m2.dev = 0; m2.base = 30000; m2.sectors = 8000;
    CHECK(mwfs_mkfs(&m2, "DATA") == MWFS_OK);
    CHECK(vfs_mount_root(0, 100, 20000) == MWFS_OK);
    mwfs_strcpy(vfs_cwd, "/", sizeof vfs_cwd);
    CHECK(vfs_mkdir("/x") == MWFS_OK);
    CHECK(vfs_mkdir("/x/y") == MWFS_OK);
    CHECK(vfs_mkdir("/x/y/z/a") == MWFS_E_NOENT);                       /* parents must exist */
    CHECK(vfs_chdir("/x/y") == MWFS_OK);
    CHECK(strcmp(vfs_cwd, "/x/y") == 0);
    u8 d[5] = { 1, 2, 3, 4, 5 };
    CHECK(vfs_write("f.txt", d, 5, 9) == MWFS_OK);                       /* relative to the cwd */
    vfs_node_t n;
    CHECK(vfs_stat("/x/y/F.TXT", &n) == MWFS_OK && n.type == MWFS_T_FILE && vfs_size(&n) == 5);
    CHECK(vfs_chdir("f.txt") == MWFS_E_NOTDIR);
    CHECK(vfs_chdir("..") == MWFS_OK && strcmp(vfs_cwd, "/x") == 0);
    CHECK(vfs_cp("y/f.txt", "/docs") == MWFS_OK);                        /* into a directory keeps the name */
    CHECK(vfs_stat("/docs/f.txt", &n) == MWFS_OK);
    CHECK(vfs_mv("/docs/f.txt", "/progs/g.txt") == MWFS_OK);
    CHECK(vfs_stat("/docs/f.txt", &n) == MWFS_E_NOENT && vfs_stat("/progs/g.txt", &n) == MWFS_OK);
    CHECK(vfs_rm("/x") == MWFS_E_NOTEMPTY);
    CHECK(vfs_rm("/") == MWFS_E_PERM && vfs_rm("/mnt") == MWFS_E_PERM);
    CHECK(vfs_mkdir("/mnt/foo") == MWFS_E_PERM);
    /* mounting */
    CHECK(vfs_mount_region("ide200", 0, 30000, 8000, 0) == MWFS_OK);
    CHECK(vfs_mount_region("IDE200", 0, 30000, 8000, 0) == MWFS_E_EXIST);
    CHECK(vfs_mount_region("sata101", 0, 40000, 8000, 0) == MWFS_E_NOFS);
    CHECK(vfs_stat("/mnt/ide200", &n) == MWFS_OK && n.is_mountpoint && n.vol == 1);
    CHECK(vfs_write("/mnt/ide200/hello.txt", d, 5, 0) == MWFS_OK);
    CHECK(vfs_mkdir("/mnt/ide200/sub") == MWFS_OK);
    CHECK(vfs_cp("/x/y/f.txt", "/mnt/ide200/sub/copy.txt") == MWFS_OK);   /* across volumes */
    CHECK(vfs_mv("/mnt/ide200/sub/copy.txt", "/docs/moved.txt") == MWFS_OK);
    CHECK(vfs_stat("/mnt/ide200/sub/copy.txt", &n) == MWFS_E_NOENT && vfs_stat("/docs/moved.txt", &n) == MWFS_OK && vfs_size(&n) == 5);
    CHECK(vfs_mv("/mnt/ide200/sub", "/docs/sub") == MWFS_E_XDEV);
    mwfs_dirent_t e; vfs_node_t mnt;
    CHECK(vfs_stat("/mnt", &mnt) == MWFS_OK && mnt.is_mnt_dir);
    CHECK(vfs_ls_nth(&mnt, 0, &e) == 1 && strcmp(e.name, "ide200") == 0 && vfs_ls_nth(&mnt, 1, &e) == 0);
    CHECK(vfs_chdir("/mnt/ide200/sub") == MWFS_OK);
    CHECK(vfs_unmount("ide200") == MWFS_OK);
    CHECK(strcmp(vfs_cwd, "/") == 0);                                    /* the terminal was standing inside: back to / */
    CHECK(vfs_stat("/mnt/ide200", &n) == MWFS_E_NOENT);
    CHECK(vfs_ls_nth(&mnt, 0, &e) == 0);
    CHECK(vfs_unmount("ide200") == MWFS_E_NOENT);
    /* deep nesting */
    char path[VFS_PATH_MAX] = "";
    int ok = 1;
    for (int i = 0; i < 12; i++) { strcat(path, "/d"); char nm[8]; snprintf(nm, sizeof nm, "%d", i); strcat(path, nm); ok &= vfs_mkdir(path) == MWFS_OK; }
    CHECK(ok);
    CHECK(vfs_chdir(path) == MWFS_OK);
    CHECK(vfs_chdir("../../..") == MWFS_OK);
    mwfs_vol_t *root = &vfs_vols[0];
    CHECK(clean(root, 0));
}

/* An image formatted and filled by tools/mwfs.py (the build's formatter) must open, list and check clean with the C code. */
static void test_python_image(void) {
    const char *path = getenv("MWFS_PY_IMAGE");
    if (!path) { printf("python image: skipped (MWFS_PY_IMAGE not set)\n"); return; }
    printf("python image\n");
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  cannot open %s\n", path); fails++; return; }
    size_t n = fread(disk + (size_t)0, 1, (size_t)DISK_SECTORS * 512, f);
    fclose(f);
    CHECK(n >= 2048 * 512);
    CHECK(vfs_mount_root(0, 1733, 315) == MWFS_OK);
    vfs_node_t nd;
    CHECK(vfs_stat("/progs/GREETER.MWP", &nd) == MWFS_OK && nd.type == MWFS_T_FILE);
    CHECK(vfs_size(&nd) > 100);
    mwfs_ino_t t; CHECK(mwfs_ino_get(&vfs_vols[0], nd.ino, &t) && t.aux == 0);
    CHECK(mwfs_file_verify(&vfs_vols[0], nd.ino));
    CHECK(vfs_stat("/docs", &nd) == MWFS_OK && vfs_stat("/mnt", &nd) == MWFS_OK && vfs_stat("/sys", &nd) == MWFS_OK);
    CHECK(clean(&vfs_vols[0], 0));
    /* and the kernel can write to it */
    u8 d[4] = { 1, 2, 3, 4 };
    CHECK(vfs_write("/docs/NEW.TXT", d, 4, 0) == MWFS_OK);
    CHECK(clean(&vfs_vols[0], 0));
}

static void test_open_errors(void) {
    printf("open errors\n");
    memset(disk, 0, (size_t)DISK_SECTORS * 512);
    mwfs_vol_t v; memset(&v, 0, sizeof v); v.dev = 0; v.base = 100; v.sectors = 4000;
    CHECK(mwfs_open(&v) == MWFS_E_NOFS);
    CHECK(mwfs_mkfs(&v, "A") == MWFS_OK);
    memset(&v, 0, sizeof v); v.dev = 0; v.base = 100; v.sectors = 4000;
    CHECK(mwfs_open(&v) == MWFS_OK);
    disk[(size_t)(100 + 0) * 512 + 20] ^= 0xFF;                                  /* a flipped bit in the superblock */
    memset(&v, 0, sizeof v); v.dev = 0; v.base = 100; v.sectors = 4000;
    CHECK(mwfs_open(&v) == MWFS_E_CORRUPT);
    memset(&v, 0, sizeof v); v.dev = 0; v.base = 100; v.sectors = 30;
    CHECK(mwfs_mkfs(&v, "T") == MWFS_E_SMALL);
    /* a volume that claims to be bigger than the region it is given */
    memset(&v, 0, sizeof v); v.dev = 0; v.base = 100; v.sectors = 5000; mwfs_mkfs(&v, "B");
    memset(&v, 0, sizeof v); v.dev = 0; v.base = 100; v.sectors = 4000;
    CHECK(mwfs_open(&v) == MWFS_E_CORRUPT);
}

int main(void) {
    disk = calloc(DISK_SECTORS, 512);
    test_basic(); test_dirs(); test_full(); test_random(); test_crash(); test_vfs(); test_python_image(); test_open_errors();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
