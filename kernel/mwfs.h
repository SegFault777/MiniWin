/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MWFS_H
#define MWFS_H
#include "io.h"

/* mwfs.h -- MWFS, the MiniWin File System: a small real filesystem (directories of any depth, files that grow, a free-space
 * bitmap) that lives inside any run of sectors -- a partition, a whole disk, or the tail of the install image. It replaces
 * the four fixed document slots and four fixed program slots of 1.1-alpha-8.
 *
 * On disk, all sizes in 512-byte sectors, all offsets relative to the start of the volume:
 *      0                     superblock: magic "MWF2", version, geometry, label, CRC-32 -- written LAST by mkfs, so a
 *                            half-formatted volume simply has no superblock
 *      bitmap_lba            free-space bitmap: one bit per sector of the volume (1 = in use), metadata included
 *      itable_lba            inode table: 64-byte inodes, 8 to a sector, inode 0 unused, inode 1 = the root directory
 *      data_lba..            data: file contents, directories, indirect blocks
 * An inode: type (free/file/dir), size, CRC-32 of a file's bytes, one free 32-bit field for the owner (programs keep their
 * entry offset there), eight direct sector pointers, one single-indirect and one double-indirect pointer -- a file can be
 * (8 + 128 + 128*128) sectors = 8.1 MB, far past anything a 25 KB program or 4 KB note will ever need.
 * A directory is a file of 64-byte entries (inode number, type, name of up to 31 characters), 8 to a sector.
 *
 * CRASH SAFETY, honestly: there is no journal. A save never modifies a file in place: the new contents go to a freshly
 * allocated inode, and only when they are complete does ONE directory-entry sector write switch the name over to it (the old
 * inode is freed afterwards). Cut the power before that write and the old file is intact; after it, the new one is. What a
 * crash can leave behind is garbage -- an inode or sectors nobody points at -- never a half-written file; mwfs_check()
 * finds that garbage and (with fix) gives it back. Directory edits are one-sector writes too. */

#define MWFS_MAGIC        0x3246574Du            /* "MWF2" little-endian */
#define MWFS_VERSION      1
#define MWFS_INO_SIZE     64
#define MWFS_NDIRECT      8
#define MWFS_PTRS         128                    /* sector pointers per indirect sector */
#define MWFS_NAME_MAX     31
#define MWFS_DENT_SIZE    64
#define MWFS_MAX_INODES   16384
#define MWFS_MIN_INODES   32
#define MWFS_ROOT_INO     1

#define MWFS_T_FREE 0
#define MWFS_T_FILE 1
#define MWFS_T_DIR  2

#define MWFS_OK          0
#define MWFS_E_IO        1
#define MWFS_E_FULL      2
#define MWFS_E_NOENT     3
#define MWFS_E_EXIST     4
#define MWFS_E_NOTDIR    5
#define MWFS_E_ISDIR     6
#define MWFS_E_NOTEMPTY  7
#define MWFS_E_NAME      8
#define MWFS_E_CORRUPT   9
#define MWFS_E_TOOBIG    10
#define MWFS_E_NOFS      11                      /* no MWFS superblock here */
#define MWFS_E_SMALL     12
#define MWFS_E_PERM      13                      /* a mount point or other place that is not a normal directory */
#define MWFS_E_XDEV      14

static inline const char *mwfs_err_text(int e) {
    switch (e) {
        case MWFS_OK:        return "ok";
        case MWFS_E_IO:      return "disk read/write failed";
        case MWFS_E_FULL:    return "no space left on the volume";
        case MWFS_E_NOENT:   return "no such file or directory";
        case MWFS_E_EXIST:   return "already exists";
        case MWFS_E_NOTDIR:  return "not a directory";
        case MWFS_E_ISDIR:   return "is a directory";
        case MWFS_E_NOTEMPTY:return "directory is not empty";
        case MWFS_E_NAME:    return "bad name (A-Z a-z 0-9 . _ - , up to 31)";
        case MWFS_E_CORRUPT: return "the filesystem is damaged (see part check)";
        case MWFS_E_TOOBIG:  return "file too big";
        case MWFS_E_NOFS:    return "no MiniWin filesystem there (see part format)";
        case MWFS_E_SMALL:   return "volume too small";
        case MWFS_E_PERM:    return "not allowed there";
        case MWFS_E_XDEV:    return "that would cross volumes";
    }
    return "error";
}

typedef struct {
    u8  used;
    int dev;                                     /* blk.h device index */
    u32 base;                                    /* absolute first sector */
    u32 sectors;                                 /* length, as the superblock says */
    u32 inode_count, bitmap_lba, bitmap_sectors, itable_lba, itable_sectors, data_lba;
    u32 cur_blk, cur_ino;                        /* next-fit allocation cursors */
    char id[16];                                 /* "ide101", or "root" */
    char label[16];
} mwfs_vol_t;

typedef struct {
    u16 type, flags;
    u32 size, crc, aux, nblocks;
    u32 direct[MWFS_NDIRECT];
    u32 ind, dind, gen;
} mwfs_ino_t;                                    /* exactly 64 bytes */
typedef char mwfs_assert_inode_size[sizeof(mwfs_ino_t) == MWFS_INO_SIZE ? 1 : -1];


/* ---------------------------------------------------------------- modified time
 * The inode's `gen` field was reserved from day one and never used, so it carries the "modified" stamp -- no format change:
 * an old disk simply has 0 there, which MiniFiles shows as a dash. The stamp is the FAT/DOS packed date-time (year-1980 in
 * bits 25-31, month 21-24, day 16-20, hour 11-15, minute 5-10, seconds/2 in 0-4) because it sorts as a plain integer.
 * mwfs.h has no clock of its own (it must build on the host too): the kernel hands it one with mwfs_clock_fn. */
static u32 (*mwfs_clock_fn)(void) = 0;
static inline u32 mwfs_now(void) { return mwfs_clock_fn ? mwfs_clock_fn() : 0; }
/* ---------------------------------------------------------------- small helpers */
static inline void mwfs_zero(void *p, u32 n) { u8 *b = (u8 *)p; for (u32 i = 0; i < n; i++) b[i] = 0; }
static inline void mwfs_copy(void *d, const void *s, u32 n) { u8 *a = (u8 *)d; const u8 *b = (const u8 *)s; for (u32 i = 0; i < n; i++) a[i] = b[i]; }
static inline u32 mwfs_crc32_update(u32 crc, const u8 *data, u32 n) {
    for (u32 i = 0; i < n; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (u32)(-(i32)(crc & 1u)));
    }
    return crc;
}
static inline int mwfs_name_ok(const char *name) {
    int n = 0;
    for (; name[n]; n++) {
        char c = name[n];
        /* Space and parentheses are allowed from alpha-10 so that "DOC (1).TXT" (what Notepad names a second new document) is a
         * legal name -- but never a leading or trailing space, which nobody can see or type reliably. MiniShell takes such names
         * in double quotes. */
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == ',' || c == ' ' || c == '(' || c == ')';
        if (!ok || n >= MWFS_NAME_MAX) return 0;
    }
    return n > 0 && name[0] != '.' && name[0] != ' ' && name[n - 1] != ' ';
}
static inline int mwfs_name_eq(const char *a, const char *b) {          /* case-insensitive, like the DOS names these imitate */
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static inline void mwfs_strcpy(char *dst, const char *src, u32 n) {
    u32 i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
}

static inline int mwfs_rd(mwfs_vol_t *v, u32 lba, void *buf) { return lba < v->sectors && blk_read(v->dev, v->base + lba, buf); }
static inline int mwfs_wr(mwfs_vol_t *v, u32 lba, const void *buf) { return lba < v->sectors && blk_write(v->dev, v->base + lba, buf); }

/* ---------------------------------------------------------------- the free-space bitmap */
static inline int mwfs_bit_get(mwfs_vol_t *v, u32 lba) {                /* 1 / 0, or -1 on an I/O error */
    u8 b[512];
    if (!mwfs_rd(v, v->bitmap_lba + lba / 4096, b)) return -1;
    u32 off = lba % 4096;
    return (b[off >> 3] >> (off & 7)) & 1;
}
static inline int mwfs_bit_set(mwfs_vol_t *v, u32 lba, int on) {
    u8 b[512];
    u32 s = v->bitmap_lba + lba / 4096, off = lba % 4096;
    if (!mwfs_rd(v, s, b)) return 0;
    if (on) b[off >> 3] |= (u8)(1u << (off & 7)); else b[off >> 3] &= (u8)~(1u << (off & 7));
    return mwfs_wr(v, s, b);
}
/* Next-fit allocation: scans the bitmap from where the last allocation ended, wraps once. 0 = no free sector (a real
 * data sector is never 0: the superblock lives there). */
static inline u32 mwfs_alloc(mwfs_vol_t *v) {
    u32 nsec = v->bitmap_sectors;
    u32 start = v->cur_blk / 4096;
    if (start >= nsec) start = 0;
    for (u32 pass = 0; pass < nsec; pass++) {
        u32 si = (start + pass) % nsec;
        u8 b[512];
        if (!mwfs_rd(v, v->bitmap_lba + si, b)) return 0;
        for (u32 i = 0; i < 512; i++) {
            if (b[i] == 0xFF) continue;
            for (u32 k = 0; k < 8; k++) {
                if (b[i] & (1u << k)) continue;
                u32 lba = si * 4096 + i * 8 + k;
                if (lba >= v->sectors) return 0;                         /* the rest of the bitmap is past the end */
                if (lba < v->data_lba) continue;
                b[i] |= (u8)(1u << k);
                if (!mwfs_wr(v, v->bitmap_lba + si, b)) return 0;
                v->cur_blk = lba + 1;
                return lba;
            }
        }
    }
    return 0;
}
static inline void mwfs_free_sector(mwfs_vol_t *v, u32 lba) {
    if (lba >= v->data_lba && lba < v->sectors) mwfs_bit_set(v, lba, 0);
}

/* ---------------------------------------------------------------- inodes */
static inline int mwfs_ino_get(mwfs_vol_t *v, u32 ino, mwfs_ino_t *out) {
    u8 b[512];
    if (ino == 0 || ino >= v->inode_count) return 0;
    if (!mwfs_rd(v, v->itable_lba + ino / 8, b)) return 0;
    mwfs_copy(out, b + (ino % 8) * MWFS_INO_SIZE, MWFS_INO_SIZE);
    return 1;
}
static inline int mwfs_ino_put(mwfs_vol_t *v, u32 ino, const mwfs_ino_t *in) {
    u8 b[512];
    if (ino == 0 || ino >= v->inode_count) return 0;
    if (!mwfs_rd(v, v->itable_lba + ino / 8, b)) return 0;
    mwfs_copy(b + (ino % 8) * MWFS_INO_SIZE, in, MWFS_INO_SIZE);
    return mwfs_wr(v, v->itable_lba + ino / 8, b);
}
static inline u32 mwfs_ino_alloc(mwfs_vol_t *v, u16 type) {
    u32 count = v->inode_count;
    for (u32 pass = 0; pass < count; pass++) {
        u32 ino = 2 + (v->cur_ino + pass) % (count - 2);
        mwfs_ino_t t;
        if (!mwfs_ino_get(v, ino, &t)) return 0;
        if (t.type == MWFS_T_FREE) {
            mwfs_zero(&t, sizeof t);
            t.type = type;
            if (!mwfs_ino_put(v, ino, &t)) return 0;
            v->cur_ino = ino;
            return ino;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- mapping a file's sectors */
static inline int mwfs_new_zeroed(mwfs_vol_t *v, u32 *out) {
    u8 z[512];
    mwfs_zero(z, 512);
    u32 l = mwfs_alloc(v);
    if (!l) return MWFS_E_FULL;
    if (!mwfs_wr(v, l, z)) return MWFS_E_IO;
    *out = l;
    return MWFS_OK;
}
/* The sector holding the idx-th sector of the file (0 = a hole). create: allocate what is missing (the data sector itself
 * is NOT zeroed unless zero_data -- a caller about to overwrite all 512 bytes skips that write). */
static inline int mwfs_bmap(mwfs_vol_t *v, mwfs_ino_t *ino, u32 idx, int create, int zero_data, u32 *out) {
    *out = 0;
    u32 *slot = 0;
    u32 table[MWFS_PTRS];
    u32 tlba = 0;
    int e;
    if (idx < MWFS_NDIRECT) {
        slot = &ino->direct[idx];
    } else if (idx - MWFS_NDIRECT < MWFS_PTRS) {
        if (!ino->ind) {
            if (!create) return MWFS_OK;
            if ((e = mwfs_new_zeroed(v, &ino->ind)) != MWFS_OK) return e;
        }
        tlba = ino->ind;
        if (!mwfs_rd(v, tlba, table)) return MWFS_E_IO;
        slot = &table[idx - MWFS_NDIRECT];
    } else {
        u32 j = idx - MWFS_NDIRECT - MWFS_PTRS;
        if (j >= MWFS_PTRS * MWFS_PTRS) return MWFS_E_TOOBIG;
        if (!ino->dind) {
            if (!create) return MWFS_OK;
            if ((e = mwfs_new_zeroed(v, &ino->dind)) != MWFS_OK) return e;
        }
        u32 top[MWFS_PTRS];
        if (!mwfs_rd(v, ino->dind, top)) return MWFS_E_IO;
        u32 *mid = &top[j / MWFS_PTRS];
        if (!*mid) {
            if (!create) return MWFS_OK;
            if ((e = mwfs_new_zeroed(v, mid)) != MWFS_OK) return e;
            if (!mwfs_wr(v, ino->dind, top)) return MWFS_E_IO;
        }
        tlba = *mid;
        if (!mwfs_rd(v, tlba, table)) return MWFS_E_IO;
        slot = &table[j % MWFS_PTRS];
    }
    if (!*slot) {
        if (!create) return MWFS_OK;
        if (zero_data) { if ((e = mwfs_new_zeroed(v, slot)) != MWFS_OK) return e; }
        else { *slot = mwfs_alloc(v); if (!*slot) return MWFS_E_FULL; }
        ino->nblocks++;
        if (tlba && !mwfs_wr(v, tlba, table)) return MWFS_E_IO;
    }
    *out = *slot;
    return MWFS_OK;
}

/* Reads up to len bytes at off; returns the number of bytes read (0 at/after the end), or -1 on an I/O error. */
static inline int mwfs_pread(mwfs_vol_t *v, mwfs_ino_t *ino, u32 off, u8 *buf, u32 len) {
    if (off >= ino->size) return 0;
    if (len > ino->size - off) len = ino->size - off;
    u32 done = 0;
    while (done < len) {
        u32 pos = off + done, idx = pos / 512, in = pos % 512, n = 512 - in;
        if (n > len - done) n = len - done;
        u32 lba;
        if (mwfs_bmap(v, ino, idx, 0, 0, &lba) != MWFS_OK) return -1;
        u8 b[512];
        if (lba) { if (!mwfs_rd(v, lba, b)) return -1; } else mwfs_zero(b, 512);
        mwfs_copy(buf + done, b + in, n);
        done += n;
    }
    return (int)done;
}

/* Writes len bytes at off (extending the file). The inode is updated in memory; the caller stores it with mwfs_ino_put(). */
static inline int mwfs_pwrite(mwfs_vol_t *v, mwfs_ino_t *ino, u32 off, const u8 *data, u32 len) {
    u32 done = 0;
    while (done < len) {
        u32 pos = off + done, idx = pos / 512, in = pos % 512, n = 512 - in;
        if (n > len - done) n = len - done;
        u32 lba;
        int e = mwfs_bmap(v, ino, idx, 1, n != 512, &lba);
        if (e != MWFS_OK) return e;
        u8 b[512];
        if (n != 512) { if (!mwfs_rd(v, lba, b)) return MWFS_E_IO; }
        mwfs_copy(b + in, data + done, n);
        if (!mwfs_wr(v, lba, b)) return MWFS_E_IO;
        done += n;
        if (pos + n > ino->size) ino->size = pos + n;
    }
    return MWFS_OK;
}

/* Gives back every sector a file or directory owns (data and indirect), and empties the inode's pointers. */
static inline void mwfs_free_blocks(mwfs_vol_t *v, mwfs_ino_t *ino) {
    for (int i = 0; i < MWFS_NDIRECT; i++) { if (ino->direct[i]) mwfs_free_sector(v, ino->direct[i]); ino->direct[i] = 0; }
    u32 t[MWFS_PTRS];
    if (ino->ind) {
        if (mwfs_rd(v, ino->ind, t)) for (int i = 0; i < MWFS_PTRS; i++) if (t[i]) mwfs_free_sector(v, t[i]);
        mwfs_free_sector(v, ino->ind); ino->ind = 0;
    }
    if (ino->dind) {
        u32 top[MWFS_PTRS];
        if (mwfs_rd(v, ino->dind, top)) {
            for (int j = 0; j < MWFS_PTRS; j++) {
                if (!top[j]) continue;
                if (mwfs_rd(v, top[j], t)) for (int i = 0; i < MWFS_PTRS; i++) if (t[i]) mwfs_free_sector(v, t[i]);
                mwfs_free_sector(v, top[j]);
            }
        }
        mwfs_free_sector(v, ino->dind); ino->dind = 0;
    }
    ino->size = 0; ino->nblocks = 0; ino->crc = 0;
}
static inline void mwfs_ino_release(mwfs_vol_t *v, u32 ino) {            /* free an inode and everything under it */
    mwfs_ino_t t;
    if (!mwfs_ino_get(v, ino, &t)) return;
    mwfs_free_blocks(v, &t);
    mwfs_zero(&t, sizeof t);
    mwfs_ino_put(v, ino, &t);
}

/* ---------------------------------------------------------------- directories */
typedef struct { u32 ino; u8 type; char name[MWFS_NAME_MAX + 1]; } mwfs_dirent_t;

/* idx-th USED entry of a directory. 1 = filled in, 0 = no more, -1 = error. */
static inline int mwfs_dir_nth(mwfs_vol_t *v, u32 dino, u32 idx, mwfs_dirent_t *out) {
    mwfs_ino_t d;
    if (!mwfs_ino_get(v, dino, &d)) return -1;
    if (d.type != MWFS_T_DIR) return -1;
    u32 seen = 0;
    for (u32 s = 0; s * 512 < d.size; s++) {
        u32 lba;
        if (mwfs_bmap(v, &d, s, 0, 0, &lba) != MWFS_OK || !lba) return -1;
        u8 b[512];
        if (!mwfs_rd(v, lba, b)) return -1;
        for (u32 e = 0; e < 8; e++) {
            const u8 *p = b + e * MWFS_DENT_SIZE;
            u32 ino = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
            if (!ino) continue;
            if (seen++ == idx) {
                out->ino = ino; out->type = p[4];
                u32 nl = p[5]; if (nl > MWFS_NAME_MAX) nl = MWFS_NAME_MAX;
                for (u32 k = 0; k < nl; k++) out->name[k] = (char)p[6 + k];
                out->name[nl] = 0;
                return 1;
            }
        }
    }
    return 0;
}

/* Every USED entry of a directory in ONE pass over its sectors (mwfs_dir_nth() restarts from the first sector for each index, so
 * listing n entries with it reads O(n^2/8) sectors -- fine for a shell command, not for a file manager redrawing a folder).
 * fn returns 0 to stop early. 0 = walked (or stopped), -1 = the directory could not be read. */
typedef int (*mwfs_dirent_fn)(const mwfs_dirent_t *e, void *ctx);
static inline int mwfs_dir_each(mwfs_vol_t *v, u32 dino, mwfs_dirent_fn fn, void *ctx) {
    mwfs_ino_t d;
    if (!mwfs_ino_get(v, dino, &d) || d.type != MWFS_T_DIR) return -1;
    for (u32 s = 0; s * 512 < d.size; s++) {
        u32 lba;
        if (mwfs_bmap(v, &d, s, 0, 0, &lba) != MWFS_OK || !lba) return -1;
        u8 b[512];
        if (!mwfs_rd(v, lba, b)) return -1;
        for (u32 e = 0; e < 8; e++) {
            const u8 *p = b + e * MWFS_DENT_SIZE;
            u32 ino = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
            if (!ino) continue;
            mwfs_dirent_t out;
            out.ino = ino; out.type = p[4];
            u32 nl = p[5]; if (nl > MWFS_NAME_MAX) nl = MWFS_NAME_MAX;
            for (u32 k = 0; k < nl; k++) out.name[k] = (char)p[6 + k];
            out.name[nl] = 0;
            if (!fn(&out, ctx)) return 0;
        }
    }
    return 0;
}

static inline int mwfs_dir_find(mwfs_vol_t *v, u32 dino, const char *name, u32 *ino_out, u8 *type_out, u32 *sec_out, u32 *ent_out) {
    mwfs_ino_t d;
    if (!mwfs_ino_get(v, dino, &d)) return MWFS_E_IO;
    if (d.type != MWFS_T_DIR) return MWFS_E_NOTDIR;
    for (u32 s = 0; s * 512 < d.size; s++) {
        u32 lba;
        if (mwfs_bmap(v, &d, s, 0, 0, &lba) != MWFS_OK || !lba) return MWFS_E_CORRUPT;
        u8 b[512];
        if (!mwfs_rd(v, lba, b)) return MWFS_E_IO;
        for (u32 e = 0; e < 8; e++) {
            const u8 *p = b + e * MWFS_DENT_SIZE;
            u32 ino = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
            if (!ino) continue;
            char nm[MWFS_NAME_MAX + 1];
            u32 nl = p[5]; if (nl > MWFS_NAME_MAX) nl = MWFS_NAME_MAX;
            for (u32 k = 0; k < nl; k++) nm[k] = (char)p[6 + k];
            nm[nl] = 0;
            if (mwfs_name_eq(nm, name)) {
                if (ino_out) *ino_out = ino;
                if (type_out) *type_out = p[4];
                if (sec_out) *sec_out = lba;
                if (ent_out) *ent_out = e;
                return MWFS_OK;
            }
        }
    }
    return MWFS_E_NOENT;
}
static inline int mwfs_dir_lookup(mwfs_vol_t *v, u32 dino, const char *name, u32 *ino_out, u8 *type_out) {
    return mwfs_dir_find(v, dino, name, ino_out, type_out, 0, 0);
}

static inline void mwfs_dent_fill(u8 *p, u32 ino, u8 type, const char *name) {
    mwfs_zero(p, MWFS_DENT_SIZE);
    p[0] = (u8)ino; p[1] = (u8)(ino >> 8); p[2] = (u8)(ino >> 16); p[3] = (u8)(ino >> 24);
    p[4] = type;
    u32 nl = 0; while (name[nl] && nl < MWFS_NAME_MAX) nl++;
    p[5] = (u8)nl;
    for (u32 k = 0; k < nl; k++) p[6 + k] = (u8)name[k];
}

static inline int mwfs_dir_add(mwfs_vol_t *v, u32 dino, const char *name, u32 ino, u8 type) {
    if (!mwfs_name_ok(name)) return MWFS_E_NAME;
    mwfs_ino_t d;
    if (!mwfs_ino_get(v, dino, &d)) return MWFS_E_IO;
    if (d.type != MWFS_T_DIR) return MWFS_E_NOTDIR;
    int f = mwfs_dir_find(v, dino, name, 0, 0, 0, 0);
    if (f == MWFS_OK) return MWFS_E_EXIST;
    if (f != MWFS_E_NOENT) return f;
    for (u32 s = 0; s * 512 < d.size; s++) {                             /* a free entry in an existing sector? */
        u32 lba;
        if (mwfs_bmap(v, &d, s, 0, 0, &lba) != MWFS_OK || !lba) return MWFS_E_CORRUPT;
        u8 b[512];
        if (!mwfs_rd(v, lba, b)) return MWFS_E_IO;
        for (u32 e = 0; e < 8; e++) {
            u8 *p = b + e * MWFS_DENT_SIZE;
            if (p[0] | p[1] | p[2] | p[3]) continue;
            mwfs_dent_fill(p, ino, type, name);
            return mwfs_wr(v, lba, b) ? MWFS_OK : MWFS_E_IO;
        }
    }
    u8 b[512];                                                           /* no: grow the directory by one sector */
    mwfs_zero(b, 512);
    mwfs_dent_fill(b, ino, type, name);
    int e = mwfs_pwrite(v, &d, d.size, b, 512);
    if (!mwfs_ino_put(v, dino, &d) && e == MWFS_OK) e = MWFS_E_IO;
    return e;
}
static inline int mwfs_dir_remove(mwfs_vol_t *v, u32 dino, const char *name) {
    u32 sec, ent;
    int e = mwfs_dir_find(v, dino, name, 0, 0, &sec, &ent);
    if (e != MWFS_OK) return e;
    u8 b[512];
    if (!mwfs_rd(v, sec, b)) return MWFS_E_IO;
    mwfs_zero(b + ent * MWFS_DENT_SIZE, MWFS_DENT_SIZE);
    return mwfs_wr(v, sec, b) ? MWFS_OK : MWFS_E_IO;
}
/* Points an existing entry at another inode: the single-sector write that makes a replace atomic. */
static inline int mwfs_dir_retarget(mwfs_vol_t *v, u32 dino, const char *name, u32 ino) {
    u32 sec, ent;
    int e = mwfs_dir_find(v, dino, name, 0, 0, &sec, &ent);
    if (e != MWFS_OK) return e;
    u8 b[512];
    if (!mwfs_rd(v, sec, b)) return MWFS_E_IO;
    u8 *p = b + ent * MWFS_DENT_SIZE;
    p[0] = (u8)ino; p[1] = (u8)(ino >> 8); p[2] = (u8)(ino >> 16); p[3] = (u8)(ino >> 24);
    return mwfs_wr(v, sec, b) ? MWFS_OK : MWFS_E_IO;
}
static inline int mwfs_dir_is_empty(mwfs_vol_t *v, u32 dino) {
    mwfs_dirent_t t;
    return mwfs_dir_nth(v, dino, 0, &t) == 0;
}

/* ---------------------------------------------------------------- whole-file operations */
/* The atomic switch-over shared by every way of making a file: the new inode `ino` (content already written, inode `t` in memory)
 * is stored, then ONE directory-entry sector write makes `name` point at it, then the old file (if any) is freed. On any failure
 * the new inode is released and the old file is untouched. */
static inline int mwfs_file_install(mwfs_vol_t *v, u32 dino, const char *name, u32 ino, mwfs_ino_t *t, int err) {
    u32 old = 0; u8 oldtype = 0;
    int f = mwfs_dir_lookup(v, dino, name, &old, &oldtype);
    int e = err;
    if (!mwfs_ino_put(v, ino, t) && e == MWFS_OK) e = MWFS_E_IO;         /* stored even after a failed write, so release() sees every sector it took */
    if (e == MWFS_OK && f != MWFS_OK && f != MWFS_E_NOENT) e = f;
    if (e == MWFS_OK && f == MWFS_OK && oldtype == MWFS_T_DIR) e = MWFS_E_ISDIR;
    if (e == MWFS_OK) e = (f == MWFS_OK) ? mwfs_dir_retarget(v, dino, name, ino) : mwfs_dir_add(v, dino, name, ino, MWFS_T_FILE);
    if (e != MWFS_OK) { mwfs_ino_release(v, ino); return e; }            /* nothing switched over: give the new copy back */
    if (f == MWFS_OK) mwfs_ino_release(v, old);
    return MWFS_OK;
}
/* Creates or atomically replaces the file `name` in directory dino with `len` bytes. `aux` is the owner's free field. */
static inline int mwfs_file_put(mwfs_vol_t *v, u32 dino, const char *name, const u8 *data, u32 len, u32 aux) {
    if (!mwfs_name_ok(name)) return MWFS_E_NAME;
    u8 oldtype = 0;
    int f = mwfs_dir_lookup(v, dino, name, 0, &oldtype);
    if (f != MWFS_OK && f != MWFS_E_NOENT) return f;
    if (f == MWFS_OK && oldtype == MWFS_T_DIR) return MWFS_E_ISDIR;
    u32 ino = mwfs_ino_alloc(v, MWFS_T_FILE);
    if (!ino) return MWFS_E_FULL;
    mwfs_ino_t t;
    if (!mwfs_ino_get(v, ino, &t)) return MWFS_E_IO;
    t.aux = aux; t.gen = mwfs_now();
    int e = len ? mwfs_pwrite(v, &t, 0, data, len) : MWFS_OK;
    if (e == MWFS_OK) t.crc = ~mwfs_crc32_update(0xFFFFFFFFu, data, len);
    return mwfs_file_install(v, dino, name, ino, &t, e);
}
/* Copies the file src_ino of volume sv into directory ddir of volume dv under `name` (atomically replacing), 512 bytes at a time. */
static inline int mwfs_file_copy(mwfs_vol_t *sv, u32 src_ino, mwfs_vol_t *dv, u32 ddir, const char *name) {
    if (!mwfs_name_ok(name)) return MWFS_E_NAME;
    mwfs_ino_t s;
    if (!mwfs_ino_get(sv, src_ino, &s)) return MWFS_E_IO;
    if (s.type != MWFS_T_FILE) return MWFS_E_ISDIR;
    u8 oldtype = 0;
    int f = mwfs_dir_lookup(dv, ddir, name, 0, &oldtype);
    if (f != MWFS_OK && f != MWFS_E_NOENT) return f;
    if (f == MWFS_OK && oldtype == MWFS_T_DIR) return MWFS_E_ISDIR;
    u32 ino = mwfs_ino_alloc(dv, MWFS_T_FILE);
    if (!ino) return MWFS_E_FULL;
    mwfs_ino_t t;
    if (!mwfs_ino_get(dv, ino, &t)) return MWFS_E_IO;
    t.aux = s.aux; t.gen = mwfs_now();
    u32 crc = 0xFFFFFFFFu;
    int e = MWFS_OK;
    for (u32 off = 0; off < s.size && e == MWFS_OK; off += 512) {
        u8 b[512];
        u32 n = s.size - off; if (n > 512) n = 512;
        if (mwfs_pread(sv, &s, off, b, n) != (int)n) { e = MWFS_E_IO; break; }
        crc = mwfs_crc32_update(crc, b, n);
        e = mwfs_pwrite(dv, &t, off, b, n);
    }
    t.crc = ~crc;
    return mwfs_file_install(dv, ddir, name, ino, &t, e);
}
/* Reads a whole file into dst (at most max bytes); *len is its real size. Returns MWFS_E_TOOBIG when it does not fit. */
static inline int mwfs_file_get(mwfs_vol_t *v, u32 ino, u8 *dst, u32 max, u32 *len) {
    mwfs_ino_t t;
    if (!mwfs_ino_get(v, ino, &t)) return MWFS_E_IO;
    if (t.type != MWFS_T_FILE) return MWFS_E_ISDIR;
    if (len) *len = t.size;
    if (t.size > max) return MWFS_E_TOOBIG;
    int n = t.size ? mwfs_pread(v, &t, 0, dst, t.size) : 0;
    return (n < 0 || (u32)n != t.size) ? MWFS_E_IO : MWFS_OK;
}
/* Reads a file and compares it with the CRC stored at save time. 1 = matches, 0 = damaged or unreadable. */
static inline int mwfs_file_verify(mwfs_vol_t *v, u32 ino) {
    mwfs_ino_t t;
    if (!mwfs_ino_get(v, ino, &t) || t.type != MWFS_T_FILE) return 0;
    u32 crc = 0xFFFFFFFFu;
    for (u32 off = 0; off < t.size; ) {
        u8 b[512];
        u32 n = t.size - off; if (n > 512) n = 512;
        int r = mwfs_pread(v, &t, off, b, n);
        if (r != (int)n) return 0;
        crc = mwfs_crc32_update(crc, b, n);
        off += n;
    }
    return ~crc == t.crc;
}
static inline int mwfs_mkdir(mwfs_vol_t *v, u32 dino, const char *name, u32 *ino_out) {
    if (!mwfs_name_ok(name)) return MWFS_E_NAME;
    int f = mwfs_dir_lookup(v, dino, name, 0, 0);
    if (f == MWFS_OK) return MWFS_E_EXIST;
    if (f != MWFS_E_NOENT) return f;
    u32 ino = mwfs_ino_alloc(v, MWFS_T_DIR);
    if (!ino) return MWFS_E_FULL;
    { mwfs_ino_t dt; if (mwfs_ino_get(v, ino, &dt)) { dt.gen = mwfs_now(); mwfs_ino_put(v, ino, &dt); } }   /* the stamp is a nicety: a failed write here is not an error */
    int e = mwfs_dir_add(v, dino, name, ino, MWFS_T_DIR);
    if (e != MWFS_OK) { mwfs_ino_release(v, ino); return e; }
    if (ino_out) *ino_out = ino;
    return MWFS_OK;
}
/* Removes a file, or an EMPTY directory. The entry goes first (so a crash leaves garbage, not a dangling name). */
static inline int mwfs_unlink(mwfs_vol_t *v, u32 dino, const char *name) {
    u32 ino; u8 type;
    int e = mwfs_dir_lookup(v, dino, name, &ino, &type);
    if (e != MWFS_OK) return e;
    if (type == MWFS_T_DIR && !mwfs_dir_is_empty(v, ino)) return MWFS_E_NOTEMPTY;
    e = mwfs_dir_remove(v, dino, name);
    if (e != MWFS_OK) return e;
    mwfs_ino_release(v, ino);
    return MWFS_OK;
}
/* Moves/renames inside one volume. The new entry is written first, the old one removed second: a crash in between leaves
 * the object reachable under BOTH names, never under none. An existing FILE at the destination is replaced. */
static inline int mwfs_rename(mwfs_vol_t *v, u32 from_dir, const char *from, u32 to_dir, const char *to) {
    u32 ino; u8 type;
    int e = mwfs_dir_lookup(v, from_dir, from, &ino, &type);
    if (e != MWFS_OK) return e;
    if (!mwfs_name_ok(to)) return MWFS_E_NAME;
    u32 dst; u8 dtype;
    int f = mwfs_dir_lookup(v, to_dir, to, &dst, &dtype);
    if (f == MWFS_OK) {
        if (dst == ino) return MWFS_OK;
        if (dtype == MWFS_T_DIR || type == MWFS_T_DIR) return MWFS_E_EXIST;
        e = mwfs_dir_retarget(v, to_dir, to, ino);
        if (e != MWFS_OK) return e;
        e = mwfs_dir_remove(v, from_dir, from);
        if (e == MWFS_OK) mwfs_ino_release(v, dst);
        return e;
    }
    if (f != MWFS_E_NOENT) return f;
    e = mwfs_dir_add(v, to_dir, to, ino, type);
    if (e != MWFS_OK) return e;
    return mwfs_dir_remove(v, from_dir, from);
}

/* ---------------------------------------------------------------- formatting and mounting */
#define MWFS_SB_CRC_OFF 44
static inline u32 mwfs_le(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline void mwfs_put(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

/* Geometry for a volume of `sectors`: also what mkfs writes. Returns 0 if the volume is too small to be useful. */
static inline int mwfs_geometry(mwfs_vol_t *v) {
    u32 n = v->sectors;
    if (n < 64) return 0;
    u32 inodes = n / 8;
    if (inodes < MWFS_MIN_INODES) inodes = MWFS_MIN_INODES;
    if (inodes > MWFS_MAX_INODES) inodes = MWFS_MAX_INODES;
    inodes = (inodes + 7) & ~7u;
    v->inode_count = inodes;
    v->bitmap_lba = 1;
    v->bitmap_sectors = (n + 4095) / 4096;
    v->itable_lba = 1 + v->bitmap_sectors;
    v->itable_sectors = inodes / 8;
    v->data_lba = v->itable_lba + v->itable_sectors;
    return v->data_lba + 8 <= n;
}

static inline int mwfs_sb_write(mwfs_vol_t *v) {
    u8 b[512];
    mwfs_zero(b, 512);
    mwfs_put(b + 0, MWFS_MAGIC); mwfs_put(b + 4, MWFS_VERSION); mwfs_put(b + 8, v->sectors);
    mwfs_put(b + 12, v->inode_count); mwfs_put(b + 16, v->bitmap_lba); mwfs_put(b + 20, v->bitmap_sectors);
    mwfs_put(b + 24, v->itable_lba); mwfs_put(b + 28, v->itable_sectors); mwfs_put(b + 32, v->data_lba);
    mwfs_put(b + 36, MWFS_ROOT_INO);
    mwfs_put(b + MWFS_SB_CRC_OFF, ~mwfs_crc32_update(0xFFFFFFFFu, b, MWFS_SB_CRC_OFF));
    for (int i = 0; i < 16; i++) b[48 + i] = (u8)v->label[i];
    return mwfs_wr(v, 0, b);
}

/* Lays a fresh, empty filesystem over the whole volume (v->dev/base/sectors must be set). Everything in the volume is lost. */
static inline int mwfs_mkfs(mwfs_vol_t *v, const char *label) {
    if (!mwfs_geometry(v)) return MWFS_E_SMALL;
    u8 z[512];
    mwfs_zero(z, 512);
    if (!mwfs_wr(v, 0, z)) return MWFS_E_IO;                             /* no superblock until the very end */
    for (u32 s = 0; s < v->bitmap_sectors; s++) {                        /* the bitmap: metadata marked used, bits past the end too */
        u8 b[512];
        mwfs_zero(b, 512);
        for (u32 bit = 0; bit < 4096; bit++) {
            u32 lba = s * 4096 + bit;
            if (lba < v->data_lba || lba >= v->sectors) b[bit >> 3] |= (u8)(1u << (bit & 7));
        }
        if (!mwfs_wr(v, v->bitmap_lba + s, b)) return MWFS_E_IO;
    }
    for (u32 s = 0; s < v->itable_sectors; s++) if (!mwfs_wr(v, v->itable_lba + s, z)) return MWFS_E_IO;
    mwfs_ino_t r;
    mwfs_zero(&r, sizeof r);
    r.type = MWFS_T_DIR;
    if (!mwfs_ino_put(v, MWFS_ROOT_INO, &r)) return MWFS_E_IO;
    mwfs_strcpy(v->label, label ? label : "", 16);
    v->cur_blk = v->data_lba; v->cur_ino = 1;
    return mwfs_sb_write(v) ? MWFS_OK : MWFS_E_IO;
}

/* Reads the superblock of the volume at v->dev/base (v->sectors = how many sectors the region really has) and fills in the
 * geometry. MWFS_E_NOFS when there is none, MWFS_E_CORRUPT when there is one that does not add up. */
static inline int mwfs_open(mwfs_vol_t *v) {
    u8 b[512];
    u32 region = v->sectors;
    if (!blk_read(v->dev, v->base, b)) return MWFS_E_IO;
    if (mwfs_le(b) != MWFS_MAGIC) return MWFS_E_NOFS;
    if (mwfs_le(b + 4) != MWFS_VERSION) return MWFS_E_CORRUPT;
    if (mwfs_le(b + MWFS_SB_CRC_OFF) != ~mwfs_crc32_update(0xFFFFFFFFu, b, MWFS_SB_CRC_OFF)) return MWFS_E_CORRUPT;
    u32 total = mwfs_le(b + 8);
    if (total > region || total < 64) return MWFS_E_CORRUPT;
    v->sectors = total;
    v->inode_count = mwfs_le(b + 12); v->bitmap_lba = mwfs_le(b + 16); v->bitmap_sectors = mwfs_le(b + 20);
    v->itable_lba = mwfs_le(b + 24); v->itable_sectors = mwfs_le(b + 28); v->data_lba = mwfs_le(b + 32);
    mwfs_vol_t g = *v;
    if (!mwfs_geometry(&g) || g.inode_count != v->inode_count || g.bitmap_lba != v->bitmap_lba || g.bitmap_sectors != v->bitmap_sectors ||
        g.itable_lba != v->itable_lba || g.itable_sectors != v->itable_sectors || g.data_lba != v->data_lba) return MWFS_E_CORRUPT;
    for (int i = 0; i < 16; i++) v->label[i] = (char)b[48 + i];
    v->label[15] = 0;
    v->cur_blk = v->data_lba; v->cur_ino = 1;
    mwfs_ino_t r;
    if (!mwfs_ino_get(v, MWFS_ROOT_INO, &r)) return MWFS_E_IO;
    if (r.type != MWFS_T_DIR) return MWFS_E_CORRUPT;
    return MWFS_OK;
}

/* Free space, in sectors, by counting zero bits (a bitmap scan: fine for `ls`/`part info`, not for a hot path). */
static inline u32 mwfs_free_count(mwfs_vol_t *v) {
    u32 free_s = 0;
    for (u32 s = 0; s < v->bitmap_sectors; s++) {
        u8 b[512];
        if (!mwfs_rd(v, v->bitmap_lba + s, b)) return free_s;
        for (u32 i = 0; i < 512; i++) {
            u8 x = (u8)~b[i];
            while (x) { free_s += x & 1u; x >>= 1; }
        }
    }
    return free_s;
}

/* ---------------------------------------------------------------- fsck */
typedef struct { u32 files, dirs, orphans, bad_entries, bad_blocks, unmarked, leaked, damaged_files, dup_links, fixed; int skipped_leak_scan, incomplete; } mwfs_report_t;

static u8 mwfs_chk_reach[MWFS_MAX_INODES / 8];

static inline int mwfs_chk_bit(const u8 *set, u32 i) { return (set[i >> 3] >> (i & 7)) & 1; }
static inline void mwfs_chk_set(u8 *set, u32 i) { set[i >> 3] |= (u8)(1u << (i & 7)); }

/* Calls fn(lba, ctx) for every sector an inode owns (data and indirect ones). Stops and returns 0 if fn returns 0. */
typedef int (*mwfs_block_fn)(u32 lba, void *ctx);
static inline int mwfs_each_block(mwfs_vol_t *v, const mwfs_ino_t *ino, mwfs_block_fn fn, void *ctx) {
    for (int i = 0; i < MWFS_NDIRECT; i++) if (ino->direct[i] && !fn(ino->direct[i], ctx)) return 0;
    u32 t[MWFS_PTRS];
    if (ino->ind) {
        if (!fn(ino->ind, ctx)) return 0;
        if (!mwfs_rd(v, ino->ind, t)) return 1;
        for (int i = 0; i < MWFS_PTRS; i++) if (t[i] && !fn(t[i], ctx)) return 0;
    }
    if (ino->dind) {
        u32 top[MWFS_PTRS];
        if (!fn(ino->dind, ctx)) return 0;
        if (!mwfs_rd(v, ino->dind, top)) return 1;
        for (int j = 0; j < MWFS_PTRS; j++) {
            if (!top[j]) continue;
            if (!fn(top[j], ctx)) return 0;
            if (!mwfs_rd(v, top[j], t)) continue;
            for (int i = 0; i < MWFS_PTRS; i++) if (t[i] && !fn(t[i], ctx)) return 0;
        }
    }
    return 1;
}

typedef struct { mwfs_vol_t *v; mwfs_report_t *r; int fix; u32 ino; } mwfs_chk_ctx_t;
static inline int mwfs_chk_block(u32 lba, void *vc) {
    mwfs_chk_ctx_t *c = (mwfs_chk_ctx_t *)vc;
    if (lba < c->v->data_lba || lba >= c->v->sectors) { c->r->bad_blocks++; return 1; }
    int bit = mwfs_bit_get(c->v, lba);
    if (bit == 0) { c->r->unmarked++; if (c->fix && mwfs_bit_set(c->v, lba, 1)) c->r->fixed++; }
    return 1;
}
#define MWFS_CHK_STACK 256
static u32 mwfs_chk_stack[MWFS_CHK_STACK];                                          /* directory stack for the iterative walk */
static inline void mwfs_chk_walk(mwfs_vol_t *v, mwfs_report_t *r, int fix) {
    int sp = 0;
    mwfs_chk_stack[sp++] = MWFS_ROOT_INO;
    mwfs_chk_set(mwfs_chk_reach, MWFS_ROOT_INO);
    while (sp) {
        u32 dino = mwfs_chk_stack[--sp];
        mwfs_ino_t d;
        if (!mwfs_ino_get(v, dino, &d)) continue;
        r->dirs++;
        mwfs_chk_ctx_t c = { v, r, fix, dino };
        mwfs_each_block(v, &d, mwfs_chk_block, &c);
        for (u32 idx = 0; ; idx++) {
            mwfs_dirent_t e;
            int n = mwfs_dir_nth(v, dino, idx, &e);
            if (n <= 0) break;
            mwfs_ino_t t;
            if (e.ino >= v->inode_count || !mwfs_ino_get(v, e.ino, &t) || t.type == MWFS_T_FREE || t.type != e.type) {
                r->bad_entries++;
                if (fix && mwfs_dir_remove(v, dino, e.name) == MWFS_OK) { r->fixed++; idx--; }
                continue;
            }
            if (mwfs_chk_bit(mwfs_chk_reach, e.ino)) {                  /* the same inode under a second name: what a crash in the middle of a rename leaves */
                r->dup_links++;
                if (fix && mwfs_dir_remove(v, dino, e.name) == MWFS_OK) { r->fixed++; idx--; }
                continue;
            }
            mwfs_chk_set(mwfs_chk_reach, e.ino);
            if (t.type == MWFS_T_DIR) { if (sp < MWFS_CHK_STACK) mwfs_chk_stack[sp++] = e.ino; else r->incomplete = 1; }
            else {
                r->files++;
                mwfs_chk_ctx_t fc = { v, r, fix, e.ino };
                mwfs_each_block(v, &t, mwfs_chk_block, &fc);
                if (!mwfs_file_verify(v, e.ino)) r->damaged_files++;
            }
        }
    }
}

typedef struct { u8 *shadow; u32 lo, hi; } mwfs_leak_ctx_t;
static inline int mwfs_leak_mark(u32 lba, void *vc) {
    mwfs_leak_ctx_t *c = (mwfs_leak_ctx_t *)vc;
    if (lba >= c->lo && lba < c->hi) c->shadow[(lba - c->lo) >> 3] |= (u8)(1u << ((lba - c->lo) & 7));
    return 1;
}

/* Checks (and with fix != 0 repairs) a mounted volume: every directory entry must point at a live inode of the right type,
 * every sector an inode owns must be inside the data area and marked used, every file must match its CRC, inodes nobody
 * points at are orphans, and used sectors nobody owns are leaks. Orphans and leaks are what a crash mid-save leaves. */
static inline int mwfs_check(mwfs_vol_t *v, int fix, mwfs_report_t *r) {
    mwfs_zero(r, sizeof *r);
    mwfs_zero(mwfs_chk_reach, sizeof mwfs_chk_reach);
    mwfs_chk_walk(v, r, fix);
    if (r->incomplete) return MWFS_OK;                                  /* the walk could not see every directory: reachability is unknown, so nothing is called an orphan or a leak */
    for (u32 ino = 2; ino < v->inode_count; ino++) {                    /* orphans */
        mwfs_ino_t t;
        if (!mwfs_ino_get(v, ino, &t)) return MWFS_E_IO;
        if (t.type == MWFS_T_FREE || mwfs_chk_bit(mwfs_chk_reach, ino)) continue;
        r->orphans++;
        if (fix) { mwfs_ino_release(v, ino); r->fixed++; }
    }
    if (v->sectors > 16u * 1024u * 1024u) { r->skipped_leak_scan = 1; return MWFS_OK; }
    for (u32 lo = v->data_lba; lo < v->sectors; lo += 32768u) {          /* leaks: window by window */
        u8 shadow[4096];
        mwfs_zero(shadow, sizeof shadow);
        u32 hi = lo + 32768u; if (hi > v->sectors) hi = v->sectors;
        mwfs_leak_ctx_t c = { shadow, lo, hi };
        for (u32 ino = 1; ino < v->inode_count; ino++) {
            if (!mwfs_chk_bit(mwfs_chk_reach, ino)) continue;
            mwfs_ino_t t;
            if (!mwfs_ino_get(v, ino, &t)) return MWFS_E_IO;
            mwfs_each_block(v, &t, mwfs_leak_mark, &c);
        }
        for (u32 lba = lo; lba < hi; lba++) {
            int owned = (shadow[(lba - lo) >> 3] >> ((lba - lo) & 7)) & 1;
            int used = mwfs_bit_get(v, lba);
            if (used == 1 && !owned) { r->leaked++; if (fix && mwfs_bit_set(v, lba, 0)) r->fixed++; }
        }
    }
    return MWFS_OK;
}
#endif
