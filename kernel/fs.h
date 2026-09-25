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
 * Disk layout (LBA = sector number, 512 bytes a pop) -- the whole image
 * is exactly 1024KB / 1MB (2048 sectors), a round number chosen on
 * purpose (see build.sh) instead of padding out to whatever happened to
 * be left over. Bumped up from 256KB once the UI's bitmap fonts moved
 * to 11x11 Galmuri11 glyphs (see kernel/font_latin_data.h,
 * kernel/font_ko_data.h) -- 11172 Hangul syllables at 11 rows of u16
 * apiece takes real space, no way around that for a kernel with no
 * font-compression scheme -- again once kernel/mwp.h needed somewhere
 * to keep loadable program binaries, and again once a bundle of real
 * (non-hand-drawn) icon bitmaps needed a home (see this file's icon
 * catalog section further down):
 *   LBA 0        - boot sector (stage 1: just enough real-mode code to
 *                  load stage 2 and jump to it -- see boot/boot.asm)
 *   LBA 1-4      - stage 2 (kernel loading, VBE truecolor mode search,
 *                  A20, GDT, the jump into protected mode -- see
 *                  boot/stage2.asm; moved out of stage 1 once finding a
 *                  real 640x480x32bpp VBE mode by actually walking the
 *                  BIOS's own mode list, instead of just requesting a
 *                  fixed mode number, stopped fitting in a 512-byte MBR)
 *   LBA 5-900    - the kernel (448KB budget; see boot/stage2.asm's
 *                  KERNEL_CHUNKS for the loader side of this same
 *                  number)
 *   LBA 901-936  - the four document slots, 9 sectors each (1 header + 8 data):
 *                    slot 0: LBA 901-909 -> "NEWDOC.TXT"
 *                    slot 1: LBA 910-918 -> "NEWDOC_2.TXT"
 *                    slot 2: LBA 919-927 -> "NEWDOC_3.TXT"
 *                    slot 3: LBA 928-936 -> "NEWDOC_4.TXT"
 *   LBA 937-1132 - the four loadable-program slots, 49 sectors each (1
 *                  header + 48 data) -- see kernel/mwp.h for the loader
 *                  that reads these:
 *                    slot 0: LBA 937-985
 *                    slot 1: LBA 986-1034
 *                    slot 2: LBA 1035-1083
 *                    slot 3: LBA 1084-1132
 *   LBA 1133-1627 - the icon catalog, 11 sectors per icon x 45 icons (1
 *                  header + 8 data sectors for a 32x32 RGBA bitmap + 2
 *                  data sectors for a 16x16 RGBA bitmap) -- see this
 *                  file's ICON_* constants and tools/install_icons.py,
 *                  the offline installer that actually writes these
 *                  (there's no in-OS icon-editor UI, so unlike the
 *                  document/program slots above, nothing at runtime
 *                  ever calls icon_save_slot() -- it exists purely so
 *                  the installer and the kernel agree on one write
 *                  path instead of the installer poking the header
 *                  format directly).
 *   LBA 1628-2047 - unused headroom (~210KB) -- room for the kernel,
 *                  the document area, the program area, or the icon
 *                  catalog to grow without immediately forcing the
 *                  image past the 1024KB line; see build.sh's size
 *                  check, which fails loudly if any of them ever does.
 * (Each area starts the sector immediately after the one before it ends
 * -- no gaps. Every sector between LBA 1 and LBA 1627 is now spoken for
 * on purpose.)
 */

#define FS_MAGIC           0x31573154u
#define FS_BASE_LBA        901
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
    /* Zeroed by hand, one byte at a time -- NOT `u8 header[512] = {0};`.
     * See prog_check_slot() below (this file's newer, sibling slot-check
     * function for loadable programs) for the full story: that shorter,
     * more obvious initializer was root-caused to intermittently
     * corrupt the very next ata_read_sector() call on this exact build,
     * and this function reads sectors the exact same way, so it gets
     * the exact same fix on general principle even though it wasn't the
     * one caught actually misbehaving. Zeroed at all so a failed read
     * reads back as "no magic found" instead of leaving the compiler
     * (rightly) suspicious that we might inspect uninitialized stack
     * garbage. */
    u8 header[512];
    for (int z = 0; z < 512; z++) header[z] = 0;
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

/* ============================================================
 * Program storage -- four slots for loadable .mwp flat binaries, kept
 * entirely separate from the document slots above (different base LBA,
 * different slot size, different name list) because a program and a
 * saved Notepad document have nothing in common except both being
 * "bytes on disk": a document is arbitrary text with no length limit
 * beyond FS_MAX_FILE_BYTES, while a program is machine code that has
 * to end up loaded at a specific address and jumped to -- see
 * kernel/mwp.h for the loader that actually does that jump. Reusing
 * the document slots for this would mean either shrinking every
 * document to fit a program-sized budget or growing every document
 * slot to a program-sized one for no reason; a second, differently-
 * sized set of slots is the honest fix.
 * ============================================================ */
#define PROG_BASE_LBA       937
#define PROG_SLOT_SECTORS   49     /* 1 header + 48 data sectors per slot */
#define PROG_DATA_SECTORS   48
#define PROG_MAX_BYTES      (PROG_DATA_SECTORS * 512)  /* 24576 bytes -- see
                             * kernel/mwp.h for why a loadable program is
                             * capped this small: it's not a filesystem
                             * limit so much as "how big a hand-written
                             * flat binary with no libc has any business
                             * being" */
#define PROG_MAX_SLOTS      4
#define PROG_NAME_MAXLEN    24     /* e.g. "GREETER.MWP" -- room to spare */

static inline u32 prog_slot_header_lba(int slot) { return PROG_BASE_LBA + (u32)slot * PROG_SLOT_SECTORS; }
static inline u32 prog_slot_data_lba(int slot)   { return prog_slot_header_lba(slot) + 1; }

/* Same magic-number-in-the-header trick as fs_check_slot() above, but a
 * different magic value on purpose -- a program slot and a document
 * slot should never be mistaken for each other even though they're
 * both "a header sector with a length in it" at a glance. The header
 * also carries an entry_offset (see kernel/mwp.h): almost always 0
 * (execution starts at the very first byte, same as this kernel's own
 * boot/stage2.asm jumping to byte 0 of the loaded kernel), but kept as
 * a real field rather than assumed, in case a future program ever
 * wants a few bytes of header/metadata of its own before its code
 * starts. */
#define PROG_MAGIC 0x50575732u /* "2WWP" little-endian -> reads "PWW2" in
                                 * a hex dump; distinct from FS_MAGIC's
                                 * "1WST" so the two slot kinds can never
                                 * collide even if something ever reads
                                 * the wrong base LBA by mistake */

static inline int prog_check_slot(int slot, u32 *out_len, u32 *out_entry_offset, char *out_name) {
    if (slot < 0 || slot >= PROG_MAX_SLOTS) return 0;
    /* Zeroed by hand, one byte at a time, rather than the more obvious
     * `u8 header[512] = {0};` -- found the hard way that this specific
     * kernel build turns that zero-initializer into an inlined bulk-zero
     * sequence (this freestanding build has no memset() to call out to,
     * so GCC synthesizes one inline) whose side effects were somehow
     * corrupting the very next inb()/outb() port I/O this function does
     * inside ata_read_sector() -- reads that reported success but
     * silently came back all-zero, reproducing on literally every
     * second call to this function within the same boot. A plain
     * counted loop compiles to ordinary stores with no such side
     * effect, and the corruption stopped completely once this replaced
     * the initializer. Exact mechanism not fully chased down (something
     * about the generated zeroing sequence and this kernel's inline-asm
     * I/O primitives disagreeing about register or flag state, most
     * likely), but the fix is confirmed solid by direct testing: see
     * this project's history for the debugging session that found it. */
    u8 header[512];
    for (int z = 0; z < 512; z++) header[z] = 0;
    if (!ata_read_sector(prog_slot_header_lba(slot), header)) return 0;
    u32 magic = *(u32*)(header + 0);
    if (magic != PROG_MAGIC) return 0;
    u32 len = *(u32*)(header + 4);
    if (len > PROG_MAX_BYTES) len = PROG_MAX_BYTES;
    if (out_len) *out_len = len;
    if (out_entry_offset) *out_entry_offset = *(u32*)(header + 8);
    if (out_name) fs_strcpy_fixed(out_name, (const char*)(header + 12), PROG_NAME_MAXLEN);
    return 1;
}

/* Writes a whole program binary into `slot` under `name`, starting
 * execution at `entry_offset` bytes into the file (0 for the overwhelming
 * common case of "the file IS the program, starting at byte 0"). Same
 * truncate-if-too-big honesty as fs_save_slot() -- a 25KB program simply
 * doesn't fit PROG_MAX_BYTES and this won't pretend otherwise. */
static inline int prog_save_slot(int slot, const char *name, const u8 *data, u32 len, u32 entry_offset) {
    if (slot < 0 || slot >= PROG_MAX_SLOTS) return 0;
    if (len > PROG_MAX_BYTES) len = PROG_MAX_BYTES;

    u8 header[512];
    fs_zero(header, 512);
    *(u32*)(header + 0) = PROG_MAGIC;
    *(u32*)(header + 4) = len;
    *(u32*)(header + 8) = entry_offset;
    fs_strcpy_fixed((char*)(header + 12), name, PROG_NAME_MAXLEN);
    if (!ata_write_sector(prog_slot_header_lba(slot), header)) return 0;

    u32 remaining = len;
    const u8 *src = data;
    u32 data_lba = prog_slot_data_lba(slot);
    for (int s = 0; s < PROG_DATA_SECTORS; s++) {
        u8 sector[512];
        fs_zero(sector, 512);
        u32 chunk = remaining > 512 ? 512 : remaining;
        for (u32 i = 0; i < chunk; i++) sector[i] = src[i];
        if (!ata_write_sector(data_lba + s, sector)) return 0;
        src += chunk;
        remaining -= chunk;
    }
    return 1;
}

/* Reads a program's bytes into dst (must have room for PROG_MAX_BYTES --
 * callers pass the fixed load-region buffer from kernel/mwp.h, never
 * something smaller). Returns how many bytes actually came back, or 0
 * if that slot's empty. */
static inline u32 prog_load_slot(int slot, u8 *dst, u32 maxlen) {
    u32 len = 0;
    if (!prog_check_slot(slot, &len, 0, 0)) return 0;
    if (len > maxlen) len = maxlen;

    u32 remaining = len;
    u8 *dstp = dst;
    u32 data_lba = prog_slot_data_lba(slot);
    for (int s = 0; s < PROG_DATA_SECTORS && remaining > 0; s++) {
        u8 sector[512];
        if (!ata_read_sector(data_lba + s, sector)) break;
        u32 chunk = remaining > 512 ? 512 : remaining;
        for (u32 i = 0; i < chunk; i++) dstp[i] = sector[i];
        dstp += chunk;
        remaining -= chunk;
    }
    return len - remaining;
}

/* Looks a program up by name (case-sensitive, exact match against
 * whatever prog_save_slot() wrote) -- this is what Terminal.mwp's `run`
 * command actually calls; it doesn't want to know slot numbers exist. */
static inline int prog_find_by_name(const char *name) {
    for (int i = 0; i < PROG_MAX_SLOTS; i++) {
        char existing[PROG_NAME_MAXLEN];
        u32 len;
        if (!prog_check_slot(i, &len, 0, existing)) continue;
        int match = 1;
        for (int j = 0; ; j++) {
            if (existing[j] != name[j]) { match = 0; break; }
            if (existing[j] == 0) break;
        }
        if (match) return i;
    }
    return -1;
}

static inline int prog_find_empty_slot(void) {
    for (int i = 0; i < PROG_MAX_SLOTS; i++) {
        u32 len;
        if (!prog_check_slot(i, &len, 0, 0)) return i;
    }
    return -1;
}

/* ============================================================
 * Icon catalog -- 45 slots of real, hand-drawn (by a person, not this
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
 * costs disk space (a 32x32 icon is 4KB, times 45 icons, is real
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
#define ICON_BASE_LBA        1133
#define ICON_SLOT_SECTORS    11    /* 1 header + 8 data (32x32) + 2 data (16x16) */
#define ICON_MAX_SLOTS       45
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
