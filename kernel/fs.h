/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FS_H
#define FS_H
#include "io.h"
#include "blk.h"
#include "mwfs.h"
#include "vfs.h"

/* fs.h -- where things live on the disk, and the document/program "slot" API the desktop, Notepad, MiniFiles and the shell
 * were written against, now a thin layer over a real filesystem (mwfs.h, vfs.h).
 *
 * Until 1.1-alpha-8 this file WAS the filesystem: four fixed document slots and four fixed program slots, each 9 or 49
 * sectors at a fixed LBA, with a write-ahead journal to make saving crash-safe. 1.1-alpha-9 replaced all of that with MWFS:
 * directories, files that grow, a free-space bitmap, and any number of documents and programs (until the disk is full).
 * What is left of "slots" is only an INDEX the UI uses: slot n is the n-th entry of a small cache of /docs (or /progs), so
 * Notepad's windows, the desktop icons and MiniFiles' rows keep pointing at "document 3" the way they always did, while the
 * bytes live in ordinary files you can also reach with ls/cat/cp/mv in MiniShell.
 *
 * Disk layout (LBA = sector, 512 bytes; the install image is exactly 1 MB = 2048 sectors):
 *   LBA 0        boot sector (stage 1, boot/boot.asm)
 *   LBA 1-4      stage 2 (boot/stage2.asm: kernel loading, VBE mode search, A20, GDT, the jump to protected mode)
 *   LBA 5-1028   the kernel (512 KB budget; boot/stage2.asm's KERNEL_CHUNKS is the loader's side of this number)
 *   LBA 1029-1732 the icon catalog: 64 icons x 11 sectors (header + 32x32 RGBA + 16x16 RGBA), written offline by
 *                tools/install_icons.py -- nothing in the running OS ever writes it
 *   LBA 1733-2047 the ROOT VOLUME, "/": an MWFS of 315 sectors in the stock image, holding /docs, /progs, /mnt and /sys.
 *                os-inst gives an installation on a bigger partition a root volume that fills the rest of the partition.
 * Every sector from LBA 1 to LBA 2047 is spoken for; build.sh checks that the kernel budget, the catalog and the root
 * volume really do follow one another with no overlap. */
#define MW_ROOTFS_LBA          1733u         /* relative to the installation's first sector */
#define MW_ROOTFS_STOCK_SECTORS 315u         /* what the stock 1 MB image has left after the catalog */

#define FS_MAX_FILE_BYTES  4096              /* a document: what Notepad's buffer holds. Bigger files are fine in the filesystem, just not in Notepad */
#define FS_MAX_FILES       64                /* how many documents the desktop/Notepad/MiniFiles index (more can exist; ls shows all) */
#define FS_NAME_MAXLEN     MWFS_NAME_MAX
#define PROG_MAX_BYTES     24576             /* a loadable program (kernel/mwp.h); the loader's slot in RAM is this big */
#define PROG_MAX_SLOTS     32
#define PROG_NAME_MAXLEN   24

static inline int fs_name_ok(const char *name) { return mwfs_name_ok(name); }
static inline int fs_name_eq(const char *a, const char *b) { return mwfs_name_eq(a, b); }
static inline void fs_zero(u8 *buf, u32 n) { for (u32 i = 0; i < n; i++) buf[i] = 0; }
static inline void fs_strcpy_fixed(char *dst, const char *src, u32 n) {
    u32 i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
}
static inline u32 fs_crc32_update(u32 crc, const u8 *data, u32 n) { return mwfs_crc32_update(crc, data, n); }

/* ============================================================ the root volume at boot */
static inline int fs_boot_mount(void) {
    u32 region = 0xFFFFFFFFu;
    if (st_scanned && blk_boot_dev >= 0 && blk_boot_dev < BLK_DEV_MAX && st_devs[blk_boot_dev].present) {
        u32 total = st_devs[blk_boot_dev].sectors;
        region = total > blk_boot_base + MW_ROOTFS_LBA ? total - blk_boot_base - MW_ROOTFS_LBA : 0;
    }
    if (region == 0xFFFFFFFFu) region = MW_ROOTFS_STOCK_SECTORS;
    return vfs_mount_root(blk_boot_dev, blk_boot_base + MW_ROOTFS_LBA, region);
}

/* ============================================================ documents: /docs, as slots */
static struct { u8 used; u32 ino, len; } fs_doc_tab[FS_MAX_FILES];
static char fs_doc_names[FS_MAX_FILES][FS_NAME_MAXLEN + 1];          /* the name of a used slot, or a name reserved for a save in progress */

static const char *fs_doc_name(int slot) {
    return (slot >= 0 && slot < FS_MAX_FILES && fs_doc_names[slot][0]) ? fs_doc_names[slot] : "DOC.TXT";
}
static inline mwfs_vol_t *fs_root(void) { return vfs_is_root_ok() ? &vfs_vols[0] : 0; }
static inline int fs_dir_ino(const char *name, u32 *ino) {
    mwfs_vol_t *v = fs_root();
    u8 t;
    if (!v) return 0;
    int e = mwfs_dir_lookup(v, MWFS_ROOT_INO, name, ino, &t);
    if (e == MWFS_E_NOENT) { u32 n; if (mwfs_mkdir(v, MWFS_ROOT_INO, name, &n) == MWFS_OK) { *ino = n; return 1; } return 0; }
    return e == MWFS_OK && t == MWFS_T_DIR;
}

/* Re-reads /docs into the slot index. Slots keep their number as long as their file keeps its name; new files take free slots. */
static inline void fs_rescan_docs(void) {
    mwfs_vol_t *v = fs_root();
    u8 seen[FS_MAX_FILES];
    for (int i = 0; i < FS_MAX_FILES; i++) seen[i] = 0;
    u32 dino;
    if (v && fs_dir_ino("docs", &dino)) {
        mwfs_dirent_t e;
        for (u32 idx = 0; mwfs_dir_nth(v, dino, idx, &e) == 1; idx++) {
            if (e.type != MWFS_T_FILE) continue;
            int slot = -1;
            for (int i = 0; i < FS_MAX_FILES; i++) if (fs_doc_names[i][0] && mwfs_name_eq(fs_doc_names[i], e.name)) { slot = i; break; }
            if (slot < 0) for (int i = 0; i < FS_MAX_FILES; i++) if (!fs_doc_names[i][0]) { slot = i; break; }
            if (slot < 0) continue;                                    /* more documents than the index holds */
            mwfs_ino_t t;
            fs_strcpy_fixed(fs_doc_names[slot], e.name, sizeof fs_doc_names[slot]);
            fs_doc_tab[slot].used = 1; fs_doc_tab[slot].ino = e.ino;
            fs_doc_tab[slot].len = mwfs_ino_get(v, e.ino, &t) ? t.size : 0;
            seen[slot] = 1;
        }
    }
    for (int i = 0; i < FS_MAX_FILES; i++) if (fs_doc_tab[i].used && !seen[i]) { fs_doc_tab[i].used = 0; fs_doc_names[i][0] = 0; }
}
static inline void fs_doc_recover(void) { fs_rescan_docs(); }         /* kept for callers: there is no journal to replay any more */

static inline int fs_check_slot(int slot, u32 *out_len) {
    if (slot < 0 || slot >= FS_MAX_FILES || !fs_doc_tab[slot].used) return 0;
    if (out_len) *out_len = fs_doc_tab[slot].len;
    return 1;
}
static inline int fs_find_by_name(const char *name) {
    for (int i = 0; i < FS_MAX_FILES; i++) if (fs_doc_tab[i].used && mwfs_name_eq(fs_doc_names[i], name)) return i;
    return -1;
}
/* A free slot with a fresh default name reserved in it (DOC.TXT, DOC (1).TXT, DOC (2).TXT, ...), or -1. */
static inline int fs_find_empty_slot(void) {
    mwfs_vol_t *v = fs_root();
    u32 dino;
    if (!v || !fs_dir_ino("docs", &dino)) return -1;
    int slot = -1;
    for (int i = 0; i < FS_MAX_FILES; i++) if (!fs_doc_names[i][0]) { slot = i; break; }
    if (slot < 0) return -1;
    for (int n = 1; n < 1000; n++) {
        char name[32]; int k = 0;
        const char *base = "DOC";
        while (*base) name[k++] = *base++;
        if (n > 1) { name[k++] = ' '; name[k++] = '('; char d[6]; int dl = 0, x = n - 1; while (x) { d[dl++] = (char)('0' + x % 10); x /= 10; } while (dl) name[k++] = d[--dl]; name[k++] = ')'; }
        const char *ext = ".TXT"; while (*ext) name[k++] = *ext++;
        name[k] = 0;
        int taken = 0;
        for (int i = 0; i < FS_MAX_FILES; i++) if (fs_doc_names[i][0] && mwfs_name_eq(fs_doc_names[i], name)) { taken = 1; break; }
        if (!taken && mwfs_dir_lookup(v, dino, name, 0, 0) == MWFS_E_NOENT) {
            fs_strcpy_fixed(fs_doc_names[slot], name, sizeof fs_doc_names[slot]);
            fs_doc_tab[slot].used = 0;
            return slot;
        }
    }
    return -1;
}
static inline int fs_store_named(int slot, const char *name, const char *data, u32 len) {
    mwfs_vol_t *v = fs_root();
    u32 dino;
    if (!v || slot < 0 || slot >= FS_MAX_FILES || len > FS_MAX_FILE_BYTES || !fs_dir_ino("docs", &dino)) return 0;
    if (mwfs_file_put(v, dino, name, (const u8 *)data, len, 0) != MWFS_OK) return 0;
    u32 ino = 0;
    mwfs_dir_lookup(v, dino, name, &ino, 0);
    if (fs_doc_tab[slot].used && !mwfs_name_eq(fs_doc_names[slot], name)) mwfs_unlink(v, dino, fs_doc_names[slot]);    /* Save As: the old name goes */
    for (int i = 0; i < FS_MAX_FILES; i++) if (i != slot && fs_doc_names[i][0] && mwfs_name_eq(fs_doc_names[i], name)) { fs_doc_tab[i].used = 0; fs_doc_names[i][0] = 0; }
    fs_strcpy_fixed(fs_doc_names[slot], name, sizeof fs_doc_names[slot]);
    fs_doc_tab[slot].used = 1; fs_doc_tab[slot].ino = ino; fs_doc_tab[slot].len = len;
    return 1;
}
static inline int fs_save_slot(int slot, const char *data, u32 len) {
    if (slot < 0 || slot >= FS_MAX_FILES || !fs_doc_names[slot][0]) return 0;
    char name[FS_NAME_MAXLEN + 1];
    fs_strcpy_fixed(name, fs_doc_names[slot], sizeof name);
    int ok = fs_store_named(slot, name, data, len);
    if (!ok && !fs_doc_tab[slot].used) fs_doc_names[slot][0] = 0;       /* a reservation that never became a file is released */
    return ok;
}
static inline int fs_save_slot_as(int slot, const char *name, const char *data, u32 len) {
    if (!fs_name_ok(name)) return 0;
    return fs_store_named(slot, name, data, len);
}
static inline int fs_delete_slot(int slot) {
    mwfs_vol_t *v = fs_root();
    u32 dino;
    if (!v || !fs_check_slot(slot, 0) || !fs_dir_ino("docs", &dino)) return 0;
    if (mwfs_unlink(v, dino, fs_doc_names[slot]) != MWFS_OK) return 0;
    fs_doc_tab[slot].used = 0; fs_doc_names[slot][0] = 0;
    return 1;
}
/* Reads a document whole: 1 and *out_len on success; 0 if it is missing, damaged (CRC mismatch) or longer than maxlen. */
static inline int fs_read_slot(int slot, char *dst, u32 maxlen, u32 *out_len) {
    mwfs_vol_t *v = fs_root();
    u32 len = 0;
    if (!v || !fs_check_slot(slot, &len) || len > maxlen) return 0;
    if (!mwfs_file_verify(v, fs_doc_tab[slot].ino)) return 0;
    if (mwfs_file_get(v, fs_doc_tab[slot].ino, (u8 *)dst, maxlen, &len) != MWFS_OK) return 0;
    if (out_len) *out_len = len;
    return 1;
}
static inline u32 fs_load_slot(int slot, char *dst, u32 maxlen) { u32 len = 0; return fs_read_slot(slot, dst, maxlen, &len) ? len : 0; }
static inline int fs_verify_slot(int slot) { mwfs_vol_t *v = fs_root(); return v && fs_check_slot(slot, 0) && mwfs_file_verify(v, fs_doc_tab[slot].ino); }

/* ============================================================ programs: /progs, as slots */
static struct { u8 used; u32 ino, len, entry; } prog_tab[PROG_MAX_SLOTS];
static char prog_names[PROG_MAX_SLOTS][PROG_NAME_MAXLEN + 1];

static inline void fs_rescan_progs(void) {
    mwfs_vol_t *v = fs_root();
    u8 seen[PROG_MAX_SLOTS];
    for (int i = 0; i < PROG_MAX_SLOTS; i++) seen[i] = 0;
    u32 dino;
    if (v && fs_dir_ino("progs", &dino)) {
        mwfs_dirent_t e;
        for (u32 idx = 0; mwfs_dir_nth(v, dino, idx, &e) == 1; idx++) {
            if (e.type != MWFS_T_FILE) continue;
            int n = 0; while (e.name[n]) n++;
            if (n > PROG_NAME_MAXLEN) continue;
            int slot = -1;
            for (int i = 0; i < PROG_MAX_SLOTS; i++) if (prog_names[i][0] && mwfs_name_eq(prog_names[i], e.name)) { slot = i; break; }
            if (slot < 0) for (int i = 0; i < PROG_MAX_SLOTS; i++) if (!prog_names[i][0]) { slot = i; break; }
            if (slot < 0) continue;
            mwfs_ino_t t;
            if (!mwfs_ino_get(v, e.ino, &t)) continue;
            fs_strcpy_fixed(prog_names[slot], e.name, sizeof prog_names[slot]);
            prog_tab[slot].used = 1; prog_tab[slot].ino = e.ino; prog_tab[slot].len = t.size; prog_tab[slot].entry = t.aux;
            seen[slot] = 1;
        }
    }
    for (int i = 0; i < PROG_MAX_SLOTS; i++) if (prog_tab[i].used && !seen[i]) { prog_tab[i].used = 0; prog_names[i][0] = 0; }
}
static inline void prog_recover(void) { }
static inline int prog_check_slot(int slot, u32 *out_len, u32 *out_entry_offset, char *out_name) {
    if (slot < 0 || slot >= PROG_MAX_SLOTS || !prog_tab[slot].used) return 0;
    if (out_len) *out_len = prog_tab[slot].len;
    if (out_entry_offset) *out_entry_offset = prog_tab[slot].entry;
    if (out_name) fs_strcpy_fixed(out_name, prog_names[slot], PROG_NAME_MAXLEN + 1);
    return 1;
}
static inline int prog_find_by_name(const char *name) {
    for (int i = 0; i < PROG_MAX_SLOTS; i++) if (prog_tab[i].used && mwfs_name_eq(prog_names[i], name)) return i;
    return -1;
}
static inline int prog_find_empty_slot(void) {
    for (int i = 0; i < PROG_MAX_SLOTS; i++) if (!prog_names[i][0]) return i;
    return -1;
}
static inline int prog_save_slot(int slot, const char *name, const u8 *data, u32 len, u32 entry_offset) {
    mwfs_vol_t *v = fs_root();
    u32 dino;
    int n = 0; while (name[n]) n++;
    if (!v || slot < 0 || slot >= PROG_MAX_SLOTS || len == 0 || len > PROG_MAX_BYTES || n > PROG_NAME_MAXLEN || entry_offset >= len || !fs_dir_ino("progs", &dino)) return 0;
    if (mwfs_file_put(v, dino, name, data, len, entry_offset) != MWFS_OK) return 0;
    fs_rescan_progs();
    return 1;
}
static inline u32 prog_load_slot(int slot, u8 *dst, u32 maxlen) {
    mwfs_vol_t *v = fs_root();
    u32 len = 0;
    if (!v || !prog_check_slot(slot, &len, 0, 0) || len == 0 || len > maxlen) return 0;
    if (!mwfs_file_verify(v, prog_tab[slot].ino)) return 0;                 /* never run bytes that do not match what was saved */
    if (mwfs_file_get(v, prog_tab[slot].ino, dst, maxlen, &len) != MWFS_OK) return 0;
    return len;
}
static inline int prog_verify_slot(int slot) { mwfs_vol_t *v = fs_root(); return v && prog_tab[slot < 0 ? 0 : slot].used && slot >= 0 && slot < PROG_MAX_SLOTS && mwfs_file_verify(v, prog_tab[slot].ino); }
static inline int prog_delete_slot(int slot) {
    mwfs_vol_t *v = fs_root();
    u32 dino;
    if (!v || !prog_check_slot(slot, 0, 0, 0) || !fs_dir_ino("progs", &dino)) return 0;
    if (mwfs_unlink(v, dino, prog_names[slot]) != MWFS_OK) return 0;
    prog_tab[slot].used = 0; prog_names[slot][0] = 0;
    return 1;
}

/* ============================================================
 * Icon catalog -- 64 slots of real, hand-drawn (by a person, not this
 * kernel) RGBA bitmap icons, replacing the hand-coded vector glyphs
 * (a few bb_rect()/bb_putpixel() calls apiece) the desktop and file
 * icons used to be drawn with. Each slot carries BOTH a 32x32 and a
 * 16x16 version of the same icon side by side, so callers (desktop
 * icons at one size, a future File Manager's list view at another) can
 * each pick the size that fits without this catalog needing to know
 * who's asking or keep two separate slot ranges in sync.
 *
 * Format is deliberately raw, uncompressed RGBA -- 4 bytes/pixel,
 * straight from the source PNGs, no palette, no run-length encoding,
 * nothing this freestanding kernel would need a decoder for. That
 * costs disk space (a 32x32 icon is 4KB, times 56 icons, is real
 * spend -- see this file's disk-layout comment for how that pushed the
 * whole image to 1MB) in exchange for a renderer that's just a pixel
 * copy loop with an alpha check, not a PNG/zlib decoder this kernel
 * has no business shipping.
 *
 * Actually blitting one of these to the screen is future work for
 * whichever pass replaces the hand-drawn desktop icon glyphs with real
 * bitmaps (see kernel/kernel.c's draw_desktop_icon() and friends) --
 * this catalog only defines the storage and the read-back API. There's
 * no icon_save_slot() exposed here on purpose: nothing in the running
 * OS ever needs to write an icon at runtime (there's no icon-editor
 * app), so the only writer is the offline tools/install_icons.py
 * installer, which pokes the same header+data layout directly rather
 * than needing a save function this file would otherwise have to
 * expose and maintain for a single external caller.
 * ============================================================ */
#define ICON_BASE_LBA        1029
#define ICON_SLOT_SECTORS    11    /* 1 header + 8 data (32x32) + 2 data (16x16) */
#define ICON_MAX_SLOTS       64    /* the current bundle has 61 distinct icons; 3 spare */
#define ICON_NAME_MAXLEN     24    /* e.g. "TYPESCRIPT_CODE" -- room to spare */

#define ICON_32_BYTES  (32 * 32 * 4)  /* 4096 bytes -- exactly 8 sectors, no padding */
#define ICON_16_BYTES  (16 * 16 * 4)  /* 1024 bytes -- exactly 2 sectors, no padding */

static inline u32 icon_slot_header_lba(int slot) { return ICON_BASE_LBA + (u32)slot * ICON_SLOT_SECTORS; }
static inline u32 icon_slot_32_lba(int slot)     { return icon_slot_header_lba(slot) + 1; }
static inline u32 icon_slot_16_lba(int slot)     { return icon_slot_32_lba(slot) + 8; }

/* Yet another distinct magic value (see PROG_MAGIC and FS_MAGIC above
 * for why each slot kind gets its own) -- "3WMI" read as a little-endian
 * hex dump, distinct from both so an icon slot, a program slot, and a
 * document slot can never be mistaken for one another even if some
 * future bug ever reads from the wrong base LBA. */
#define ICON_MAGIC 0x494D5733u

static inline int icon_check_slot(int slot, char *out_name) {
    if (slot < 0 || slot >= ICON_MAX_SLOTS) return 0;
    u8 header[512];
    for (int z = 0; z < 512; z++) header[z] = 0;
    /* See prog_check_slot()'s own comment above on why this is a hand-
     * written zeroing loop and not `u8 header[512] = {0};` -- same
     * kernel, same miscompile, same fix, applied here on the same
     * general principle even though this function wasn't the one
     * caught misbehaving. */
    if (!ata_read_sector(icon_slot_header_lba(slot), header)) return 0;
    u32 magic = *(u32*)(header + 0);
    if (magic != ICON_MAGIC) return 0;
    if (out_name) fs_strcpy_fixed(out_name, (const char*)(header + 4), ICON_NAME_MAXLEN);
    return 1;
}

/* Reads one icon's pixel data into `dst`, which must be exactly
 * ICON_32_BYTES (for want_32=1) or ICON_16_BYTES (want_32=0) long.
 * Pixel format is 4 bytes/pixel, row-major, top-to-bottom, left-to-
 * right, in the same R,G,B,A byte order the source PNGs used (see
 * tools/install_icons.py) -- not yet matched up against whatever byte
 * order kernel/vga.h's COL_* truecolor constants use internally, which
 * is exactly the kind of detail the eventual blit function (not
 * written yet) will need to get right; this function just hands back
 * the bytes as stored. Returns 1 on success, 0 if the slot's empty or
 * the read failed. */
static inline int icon_load_slot(int slot, int want_32, u8 *dst) {
    char name[ICON_NAME_MAXLEN];
    if (!icon_check_slot(slot, name)) return 0;

    u32 base_lba = want_32 ? icon_slot_32_lba(slot) : icon_slot_16_lba(slot);
    int sectors = want_32 ? 8 : 2;
    for (int s = 0; s < sectors; s++) {
        u8 sector[512];
        if (!ata_read_sector(base_lba + s, sector)) return 0;
        for (int i = 0; i < 512; i++) dst[s * 512 + i] = sector[i];
    }
    return 1;
}

/* Looks an icon up by name (case-sensitive, exact match against
 * whatever tools/install_icons.py wrote -- see that script for the
 * naming convention, e.g. "NOTEPAD", "SETTING", "WEB", "TRASHCAN",
 * "FOLDER", "PYTHON_CODE"). Returns -1 if nothing by that name was
 * ever installed. */
static inline int icon_find_by_name(const char *name) {
    for (int i = 0; i < ICON_MAX_SLOTS; i++) {
        char existing[ICON_NAME_MAXLEN];
        if (!icon_check_slot(i, existing)) continue;
        int match = 1;
        for (int j = 0; ; j++) {
            if (existing[j] != name[j]) { match = 0; break; }
            if (existing[j] == 0) break;
        }
        if (match) return i;
    }
    return -1;
}

#endif
