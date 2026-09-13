#ifndef FS_H
#define FS_H
#include "io.h"
#include "ata.h"

/* This is NOT a filesystem. Let's be honest with each other. It's four
 * fixed parking spots on disk, each with a name tag already glued on.
 * No renaming, no folders, no fanciness -- just enough structure that
 * "Save" stops eating your previous file every single time, which,
 * before this existed, it absolutely did. Saving always grabs the first
 * empty slot (or reuses the one this document already belongs to),
 * giving you NEWDOC.TXT, then _2, then _3, then _4, and then a
 * polite "sorry, full" if you try for a 5th.
 *
 * Disk layout (LBA = sector number, 512 bytes a pop):
 *   LBA 0        - boot sector
 *   LBA 1-1024   - the kernel (512KB budget -- it's got a whole Hangul
 *                  font, and a network stack is still on the way)
 *   LBA 1100+    - the four file slots, 9 sectors each (1 header + 8 data):
 *                    slot 0: LBA 1100-1108 -> "NEWDOC.TXT"
 *                    slot 1: LBA 1109-1117 -> "NEWDOC_2.TXT"
 *                    slot 2: LBA 1118-1126 -> "NEWDOC_3.TXT"
 *                    slot 3: LBA 1127-1135 -> "NEWDOC_4.TXT"
 * (There's a gap between LBA 1024 and 1100 on purpose -- room to grow
 * the kernel's own budget again later without immediately colliding
 * with the file storage area the way the old LBA-300 layout eventually
 * would have.)
 */

#define FS_MAGIC           0x31573154u
#define FS_BASE_LBA        1100
#define FS_SLOT_SECTORS    9      /* 1 header + 8 data sectors per slot */
#define FS_DATA_SECTORS    8
#define FS_MAX_FILE_BYTES  (FS_DATA_SECTORS * 512)  /* 4096 bytes, don't write a novel */
#define FS_MAX_FILES       4

static const char *fs_slot_names[FS_MAX_FILES] = {
    "NEWDOC.TXT",
    "NEWDOC_2.TXT",
    "NEWDOC_3.TXT",
    "NEWDOC_4.TXT",
};

static inline u32 fs_slot_header_lba(int slot) { return FS_BASE_LBA + (u32)slot * FS_SLOT_SECTORS; }
static inline u32 fs_slot_data_lba(int slot)   { return fs_slot_header_lba(slot) + 1; }

static inline void fs_zero(u8 *buf, u32 n) {
    for (u32 i = 0; i < n; i++) buf[i] = 0;
}

static inline void fs_strcpy_fixed(char *dst, const char *src, u32 n) {
    u32 i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
}

/* Shoves `len` bytes of `data` into the given slot (0..FS_MAX_FILES-1)
 * under that slot's fixed name. Returns 1 if the disk cooperated. */
static inline int fs_save_slot(int slot, const char *data, u32 len) {
    if (slot < 0 || slot >= FS_MAX_FILES) return 0;
    if (len > FS_MAX_FILE_BYTES) len = FS_MAX_FILE_BYTES; /* not gonna fit, tough luck, truncating */

    u8 header[512];
    fs_zero(header, 512);
    *(u32*)(header + 0) = FS_MAGIC;
    *(u32*)(header + 4) = len;
    fs_strcpy_fixed((char*)(header + 8), fs_slot_names[slot], 32);
    if (!ata_write_sector(fs_slot_header_lba(slot), header)) return 0;

    u32 remaining = len;
    const char *src = data;
    u32 data_lba = fs_slot_data_lba(slot);
    for (int s = 0; s < FS_DATA_SECTORS; s++) {
        u8 sector[512];
        fs_zero(sector, 512);
        u32 chunk = remaining > 512 ? 512 : remaining;
        for (u32 i = 0; i < chunk; i++) sector[i] = (u8)src[i];
        if (!ata_write_sector(data_lba + s, sector)) return 0;
        src += chunk;
        remaining -= chunk;
    }
    return 1;
}

/* Peeks at `slot` and tells you if there's a real file there (checks for
 * our magic number so we don't mistake random disk garbage for a save).
 * Fills *out_len if so. */
static inline int fs_check_slot(int slot, u32 *out_len) {
    if (slot < 0 || slot >= FS_MAX_FILES) return 0;
    u8 header[512];
    if (!ata_read_sector(fs_slot_header_lba(slot), header)) return 0;
    u32 magic = *(u32*)(header + 0);
    if (magic != FS_MAGIC) return 0; /* nope, just leftover zeros or noise */
    u32 len = *(u32*)(header + 4);
    if (len > FS_MAX_FILE_BYTES) len = FS_MAX_FILE_BYTES;
    if (out_len) *out_len = len;
    return 1;
}

/* Reads `slot`'s contents back into dst (up to maxlen bytes). Returns how
 * many bytes actually made it, or 0 if that slot's empty. */
static inline u32 fs_load_slot(int slot, char *dst, u32 maxlen) {
    u32 len = 0;
    if (!fs_check_slot(slot, &len)) return 0;
    if (len > maxlen) len = maxlen;

    u32 remaining = len;
    char *dstp = dst;
    u32 data_lba = fs_slot_data_lba(slot);
    for (int s = 0; s < FS_DATA_SECTORS && remaining > 0; s++) {
        u8 sector[512];
        if (!ata_read_sector(data_lba + s, sector)) break;
        u32 chunk = remaining > 512 ? 512 : remaining;
        for (u32 i = 0; i < chunk; i++) dstp[i] = (char)sector[i];
        dstp += chunk;
        remaining -= chunk;
    }
    return len - remaining;
}

/* Finds the first slot nobody's using yet. Returns -1 if all four are
 * taken, at which point it's officially "your problem now, go delete
 * something" -- except we don't have a delete feature yet either. Oops. */
static inline int fs_find_empty_slot(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        u32 len;
        if (!fs_check_slot(i, &len)) return i;
    }
    return -1;
}

#endif
