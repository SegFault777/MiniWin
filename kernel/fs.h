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
 *   LBA 5-1028   - the kernel (512KB budget, grown from 448KB for the HTML5 engine; see boot/stage2.asm's
 *                  KERNEL_CHUNKS for the loader side of this same
 *                  number)
 *   LBA 1029-1064 - the four document slots, 9 sectors each (1 header + 8 data):
 *                    slot 0: LBA 1029-1037 -> "NEWDOC.TXT"
 *                    slot 1: LBA 1038-1046 -> "NEWDOC_2.TXT"
 *                    slot 2: LBA 1047-1055 -> "NEWDOC_3.TXT"
 *                    slot 3: LBA 1056-1064 -> "NEWDOC_4.TXT"
 *   LBA 1065-1260 - the four loadable-program slots, 49 sectors each (1
 *                  header + 48 data) -- see kernel/mwp.h for the loader
 *                  that reads these:
 *                    slot 0: LBA 1065-1113
 *                    slot 1: LBA 1114-1162
 *                    slot 2: LBA 1163-1211
 *                    slot 3: LBA 1212-1260
 *   LBA 1261-1755 - the icon catalog, 11 sectors per icon x 45 icons (1
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
 *   LBA 1756-1764 - the document JOURNAL (1 header + 8 data sectors): where a save is
 *                  staged and committed before it is applied to its slot, so
 *                  an interrupted save can be completed (or never started)
 *                  instead of leaving a half-written file. See fsj_save().
 *   LBA 1765-1813 - the program journal (1 header + 48 data sectors), same idea.
 *   LBA 1814-2047 - unused headroom (~117KB) -- room for the kernel,
 *                  the document area, the program area, or the icon
 *                  catalog to grow without immediately forcing the
 *                  image past the 1024KB line; see build.sh's size
 *                  check, which fails loudly if any of them ever does.
 * (Each area starts the sector immediately after the one before it ends
 * -- no gaps. Every sector between LBA 1 and LBA 1755 is now spoken for
 * on purpose.)
 */

#define FS_MAGIC           0x31573154u
#define FS_BASE_LBA        1029
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

/* ============================================================
 * Crash-safe saves (rc-4): commit record + write-ahead journal
 *
 * THE PROBLEM. A save used to write the slot header (magic + length) FIRST and the data after. Cut the
 * power -- or fail one sector write -- in between and the next boot found a perfectly valid header over
 * half-new, half-old data: a file that looked fine and was quietly wrong (for a program, one that `run`
 * would happily execute). There was also no way to notice later that the bytes read back were not the
 * bytes that were saved.
 *
 * THE FIX, in two parts.
 *  1. COMMIT RECORD. The last 8 bytes of every document/program header sector hold the CRC-32 of the
 *     saved bytes (offset 504) and the marker FS_COMMIT_MAGIC (508). A load recomputes the CRC and refuses
 *     bytes that do not match. Headers written before this existed have zeros there ("legacy"): still
 *     accepted, just unverifiable, so no existing disk image or tool breaks.
 *  2. JOURNAL. A save never touches the live slot until the complete new copy is safely on disk:
 *        a. invalidate the journal header,
 *        b. write all data sectors into the journal area,
 *        c. write the journal header (same header the slot will get + the target slot) -- THE COMMIT POINT,
 *        d. only now invalidate the live slot, copy the data in and write its header,
 *        e. retire the journal.
 *     Power loss before (c): the live slot was never touched -- the OLD file is intact. Power loss after
 *     (c): the journal holds a complete, CRC-checked copy of the NEW file, and fs_*_recover() finishes
 *     step (d) the next time the disk is touched. A one-sector header write is the only thing that has to
 *     be atomic, which is what a drive gives you. Either the old or the new file survives -- never a mix.
 * ============================================================ */
#define FS_HDR_CRC_OFF     504
#define FS_HDR_COMMIT_OFF  508
#define FS_HDR_TARGET_OFF  496                 /* journal header only: 1 + the slot a staged save is destined for */
#define FS_COMMIT_MAGIC    0x31544D43u         /* "CMT1" little-endian */
#define FS_JOURNAL_LBA     1756                /* document journal (9 sectors) -- see the layout map */
#define PROG_JOURNAL_LBA   1765                /* program journal (49 sectors) */

/* Standard CRC-32 (IEEE 802.3, the one zlib/zip/PNG use: reflected, poly 0xEDB88320), bitwise -- no 1KB table in
 * a kernel that counts its bytes; 24KB of program is ~200K cheap operations, nothing next to the disk reads.
 * Feed it in pieces: start with 0xFFFFFFFF, pass the return value back in, invert (~) the final value. */
static inline u32 fs_crc32_update(u32 crc, const u8 *data, u32 n) {
    for (u32 i = 0; i < n; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (u32)(-(i32)(crc & 1u)));
    }
    return crc;
}

/* Stamps the commit record for `crc` (already finalized) into a header sector buffer. */
static inline void fs_hdr_set_commit(u8 *header, u32 crc) {
    *(u32*)(header + FS_HDR_CRC_OFF)    = crc;
    *(u32*)(header + FS_HDR_COMMIT_OFF) = FS_COMMIT_MAGIC;
}

/* Reads the header sector at `lba` and reports its commit record: 0 = the sector couldn't be read,
 * 1 = a legacy header with no commit record (nothing to verify), 2 = committed; *crc is set then. */
static inline int fs_read_commit(u32 lba, u32 *crc) {
    u8 header[512];
    for (int z = 0; z < 512; z++) header[z] = 0;      /* by hand, see prog_check_slot() for why */
    if (!ata_read_sector(lba, header)) return 0;
    if (*(u32*)(header + FS_HDR_COMMIT_OFF) != FS_COMMIT_MAGIC) return 1;
    *crc = *(u32*)(header + FS_HDR_CRC_OFF);
    return 2;
}

static inline int fsj_zero_header(u32 lba) {
    u8 z[512];
    for (int i = 0; i < 512; i++) z[i] = 0;
    return ata_write_sector(lba, z);
}

/* Writes `nsec` consecutive data sectors at `lba` from `src` (`len` real bytes, the rest zero padding) and
 * returns the CRC-32 of the `len` bytes through *crc_out (finalized). */
static inline int fsj_write_data(u32 lba, u32 nsec, const u8 *src, u32 len, u32 *crc_out) {
    u32 remaining = len, crc = 0xFFFFFFFFu;
    for (u32 s = 0; s < nsec; s++) {
        u8 sector[512];
        fs_zero(sector, 512);
        u32 chunk = remaining > 512 ? 512 : remaining;
        for (u32 i = 0; i < chunk; i++) sector[i] = src[i];
        crc = fs_crc32_update(crc, sector, chunk);
        if (!ata_write_sector(lba + s, sector)) return 0;
        src += chunk;
        remaining -= chunk;
    }
    if (crc_out) *crc_out = ~crc;
    return 1;
}

/* The journaled write itself, shared by documents and programs. `header` is the slot header the caller built
 * (magic, length, name...) WITHOUT the commit record; the live slot is `slot` of the area starting at
 * `base_lba` with `slot_sectors` sectors per slot (1 header + data). Returns 1 only when the live slot holds
 * the complete new file. A 0 means: before the commit point, nothing changed; after it, the journal still
 * holds the new copy and the next fsj_recover() completes it. */
static inline int fsj_save(u32 jlba, u32 base_lba, u32 slot_sectors, int slot, u8 *header, const u8 *src, u32 len) {
    u32 nsec = slot_sectors - 1;
    u32 hdr_lba = base_lba + (u32)slot * slot_sectors;
    u32 crc = 0;

    if (!fsj_zero_header(jlba)) return 0;                                  /* a. */
    if (!fsj_write_data(jlba + 1, nsec, src, len, &crc)) return 0;         /* b. */
    fs_hdr_set_commit(header, crc);
    u8 jh[512];
    for (int i = 0; i < 512; i++) jh[i] = header[i];
    *(u32*)(jh + FS_HDR_TARGET_OFF) = (u32)slot + 1;
    if (!ata_write_sector(jlba, jh)) return 0;                             /* c. COMMIT POINT */

    if (!fsj_zero_header(hdr_lba)) return 0;                               /* d. */
    if (!fsj_write_data(hdr_lba + 1, nsec, src, len, 0)) return 0;
    if (!ata_write_sector(hdr_lba, header)) return 0;

    fsj_zero_header(jlba);   /* e. best effort: a journal that stays behind only re-applies the identical bytes */
    return 1;
}

/* Finishes (or discards) whatever a previous save left in the journal. Returns 1 when the journal is now
 * settled (empty, applied, or discarded as unusable) and 0 when the disk would not cooperate (try again
 * later). A journal whose staged data fails its CRC never reached the commit point intact and is dropped. */
static inline int fsj_recover(u32 jlba, u32 base_lba, u32 slot_sectors, u32 slot_count, u32 magic) {
    u32 nsec = slot_sectors - 1;
    u8 jh[512];
    for (int z = 0; z < 512; z++) jh[z] = 0;
    if (!ata_read_sector(jlba, jh)) return 0;
    if (*(u32*)(jh + 0) != magic || *(u32*)(jh + FS_HDR_COMMIT_OFF) != FS_COMMIT_MAGIC) return 1;   /* nothing committed */

    u32 target = *(u32*)(jh + FS_HDR_TARGET_OFF);
    u32 len = *(u32*)(jh + 4);
    if (target == 0 || target > slot_count || len > nsec * 512) { fsj_zero_header(jlba); return 1; }
    target -= 1;

    u32 crc = 0xFFFFFFFFu, remaining = len;
    for (u32 s = 0; s < nsec && remaining > 0; s++) {
        u8 sector[512];
        if (!ata_read_sector(jlba + 1 + s, sector)) return 0;
        u32 chunk = remaining > 512 ? 512 : remaining;
        crc = fs_crc32_update(crc, sector, chunk);
        remaining -= chunk;
    }
    if (~crc != *(u32*)(jh + FS_HDR_CRC_OFF)) { fsj_zero_header(jlba); return 1; }

    u32 hdr_lba = base_lba + target * slot_sectors;
    if (!fsj_zero_header(hdr_lba)) return 0;
    for (u32 s = 0; s < nsec; s++) {
        u8 sector[512];
        if (!ata_read_sector(jlba + 1 + s, sector)) return 0;
        if (!ata_write_sector(hdr_lba + 1 + s, sector)) return 0;
    }
    *(u32*)(jh + FS_HDR_TARGET_OFF) = 0;                                   /* the live header carries no target */
    if (!ata_write_sector(hdr_lba, jh)) return 0;
    fsj_zero_header(jlba);
    return 1;
}

/* Lazily replays the document journal: checked on first use (and again after any failed save), so a crash
 * mid-save is repaired the next time anything looks at the files -- no separate mount step needed. */
static int fs_doc_journal_ok = 0;
static inline void fs_doc_recover(void) {
    if (fs_doc_journal_ok) return;
    if (fsj_recover(FS_JOURNAL_LBA, FS_BASE_LBA, FS_SLOT_SECTORS, FS_MAX_FILES, FS_MAGIC)) fs_doc_journal_ok = 1;
}

/* Shoves `len` bytes of `data` into the given slot (0..FS_MAX_FILES-1) under that slot's fixed name. Returns 1
 * only if EVERYTHING landed on disk, 0 otherwise -- including "doesn't fit": a document over
 * FS_MAX_FILE_BYTES used to be silently cut off and reported as saved, so the user lost the tail of their
 * text without a word. Now it is refused up front, before a single sector is touched, and the old file stays
 * exactly as it was. The write itself is journaled (see fsj_save() and the block comment above it): an
 * interrupted save leaves either the old file or the complete new one. */
static inline int fs_save_slot(int slot, const char *data, u32 len) {
    if (slot < 0 || slot >= FS_MAX_FILES) return 0;
    if (len > FS_MAX_FILE_BYTES) return 0;
    fs_doc_recover();
    if (!fs_doc_journal_ok) return 0;          /* an earlier save is still waiting to be completed and the disk won't let it */

    u8 header[512];
    fs_zero(header, 512);
    *(u32*)(header + 0) = FS_MAGIC;
    *(u32*)(header + 4) = len;
    fs_strcpy_fixed((char*)(header + 8), fs_slot_names[slot], 32);
    int ok = fsj_save(FS_JOURNAL_LBA, FS_BASE_LBA, FS_SLOT_SECTORS, slot, header, (const u8 *)data, len);
    if (!ok) fs_doc_journal_ok = 0;            /* the journal may hold a committed copy: re-check on the next access */
    return ok;
}

/* Erases a slot by zeroing its header sector -- specifically, zeroing
 * out FS_MAGIC, which is all fs_check_slot() actually looks at to
 * decide a slot is occupied. Doesn't bother touching the FS_DATA_SECTORS
 * data sectors themselves (there's no need: nothing ever reads them
 * without fs_check_slot() passing first, and leaving old bytes sitting
 * there costs nothing since the next fs_save_slot() to that slot
 * overwrites every data sector unconditionally anyway). Used by
 * Terminal.mwp's `del` command -- there's no in-OS delete UI elsewhere
 * (Notepad's own File menu only ever saves, never deletes), so this is
 * currently `del`'s only caller. */
static inline int fs_delete_slot(int slot) {
    if (slot < 0 || slot >= FS_MAX_FILES) return 0;
    fs_doc_recover();                         /* settle any pending save first, so it cannot resurrect the file */
    u8 header[512];
    fs_zero(header, 512);
    return ata_write_sector(fs_slot_header_lba(slot), header);
}
/* Returns 1 if this slot has a valid saved file: reads back the header and checks
 * our magic number so we don't mistake random disk garbage for a save.
 * Fills *out_len if so. */
static inline int fs_check_slot(int slot, u32 *out_len) {
    if (slot < 0 || slot >= FS_MAX_FILES) return 0;
    fs_doc_recover();
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
    if (len > FS_MAX_FILE_BYTES) return 0;   /* no save can ever write this: a damaged header, not a file */
    if (out_len) *out_len = len;
    return 1;
}

/* Reads `slot`'s contents back into dst. All-or-nothing, like prog_load_slot(): returns 1 and sets *out_len only
 * if the slot is valid, `maxlen` can hold the whole file, every sector read cleanly, and (for a committed slot)
 * the CRC-32 of what came back matches the one stored at save time. Anything else returns 0 -- so "this file is
 * damaged" is finally distinguishable from "this file is empty" (an empty file is a valid slot with
 * *out_len == 0), and a caller can no longer end up editing a silently truncated copy and saving THAT over the
 * original. */
static inline int fs_read_slot(int slot, char *dst, u32 maxlen, u32 *out_len) {
    u32 len = 0;
    if (!fs_check_slot(slot, &len)) return 0;
    if (len > maxlen) return 0;               /* would have to cut it short: refuse instead */

    u32 want_crc = 0;
    int commit = fs_read_commit(fs_slot_header_lba(slot), &want_crc);
    if (commit == 0) return 0;

    u32 remaining = len;
    char *dstp = dst;
    u32 data_lba = fs_slot_data_lba(slot);
    u32 crc = 0xFFFFFFFFu;
    for (int s = 0; s < FS_DATA_SECTORS && remaining > 0; s++) {
        u8 sector[512];
        if (!ata_read_sector(data_lba + s, sector)) return 0;
        u32 chunk = remaining > 512 ? 512 : remaining;
        crc = fs_crc32_update(crc, sector, chunk);
        for (u32 i = 0; i < chunk; i++) dstp[i] = (char)sector[i];
        dstp += chunk;
        remaining -= chunk;
    }
    if (remaining != 0) return 0;
    if (commit == 2 && ~crc != want_crc) return 0;   /* bytes differ from what was saved */
    if (out_len) *out_len = len;
    return 1;
}

/* The older, simpler entry point: the file's length, or 0 if the slot is empty OR unreadable (use
 * fs_read_slot() when the difference matters, as Notepad and Terminal now do). */
static inline u32 fs_load_slot(int slot, char *dst, u32 maxlen) {
    u32 len = 0;
    return fs_read_slot(slot, dst, maxlen, &len) ? len : 0;
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
#define PROG_BASE_LBA       1065
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

/* Program-area journal replay: same lazy scheme as fs_doc_recover(). */
static int fs_prog_journal_ok = 0;
static inline void prog_recover(void) {
    if (fs_prog_journal_ok) return;
    if (fsj_recover(PROG_JOURNAL_LBA, PROG_BASE_LBA, PROG_SLOT_SECTORS, PROG_MAX_SLOTS, PROG_MAGIC)) fs_prog_journal_ok = 1;
}

static inline int prog_check_slot(int slot, u32 *out_len, u32 *out_entry_offset, char *out_name) {
    if (slot < 0 || slot >= PROG_MAX_SLOTS) return 0;
    prog_recover();
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
    if (len > PROG_MAX_BYTES) return 0;   /* damaged header: no save can write this length */
    if (out_len) *out_len = len;
    if (out_entry_offset) *out_entry_offset = *(u32*)(header + 8);
    if (out_name) fs_strcpy_fixed(out_name, (const char*)(header + 12), PROG_NAME_MAXLEN);
    return 1;
}

/* Writes a whole program binary into `slot` under `name`, starting execution at `entry_offset` bytes into the
 * file (0 for the overwhelming common case of "the file IS the program, starting at byte 0"). A program over
 * PROG_MAX_BYTES is refused (returns 0, nothing written) rather than cut short and reported as saved -- a
 * program missing its tail is a crash waiting for someone to type `run`. Journaled exactly like documents (see
 * fsj_save()): an interrupted save leaves the old program or the complete new one, never half of each. */
static inline int prog_save_slot(int slot, const char *name, const u8 *data, u32 len, u32 entry_offset) {
    if (slot < 0 || slot >= PROG_MAX_SLOTS) return 0;
    if (len > PROG_MAX_BYTES) return 0;
    prog_recover();
    if (!fs_prog_journal_ok) return 0;

    u8 header[512];
    fs_zero(header, 512);
    *(u32*)(header + 0) = PROG_MAGIC;
    *(u32*)(header + 4) = len;
    *(u32*)(header + 8) = entry_offset;
    fs_strcpy_fixed((char*)(header + 12), name, PROG_NAME_MAXLEN);
    int ok = fsj_save(PROG_JOURNAL_LBA, PROG_BASE_LBA, PROG_SLOT_SECTORS, slot, header, data, len);
    if (!ok) fs_prog_journal_ok = 0;
    return ok;
}

/* Reads a program's bytes into dst (must have room for PROG_MAX_BYTES --
 * callers pass the fixed load-region buffer from kernel/mwp.h, never
 * something smaller). All-or-nothing: returns the program's full length
 * once EVERY byte has come back, and 0 if the slot's empty, the buffer
 * is too small for the whole thing, or any sector read fails along the
 * way. It used to return however many bytes it had managed before a
 * failing sector, and mwp_run() happily jumped into a half-loaded
 * program -- the unloaded tail being whatever garbage RAM held. A short
 * read is not "a smaller program", it's a broken one. */
static inline u32 prog_load_slot(int slot, u8 *dst, u32 maxlen) {
    u32 len = 0;
    if (!prog_check_slot(slot, &len, 0, 0)) return 0;
    if (len == 0 || len > maxlen) return 0;   /* truncating a program to fit is just a slower crash */

    u32 want_crc = 0;
    int commit = fs_read_commit(prog_slot_header_lba(slot), &want_crc);
    if (commit == 0) return 0;

    u32 remaining = len;
    u8 *dstp = dst;
    u32 data_lba = prog_slot_data_lba(slot);
    u32 crc = 0xFFFFFFFFu;
    for (int s = 0; s < PROG_DATA_SECTORS && remaining > 0; s++) {
        u8 sector[512];
        if (!ata_read_sector(data_lba + s, sector)) return 0;   /* partial program: refuse the whole thing */
        u32 chunk = remaining > 512 ? 512 : remaining;
        crc = fs_crc32_update(crc, sector, chunk);
        for (u32 i = 0; i < chunk; i++) dstp[i] = sector[i];
        dstp += chunk;
        remaining -= chunk;
    }
    if (remaining != 0) return 0;                  /* ran out of slot sectors before running out of program */
    if (commit == 2 && ~crc != want_crc) return 0; /* bytes differ from what was saved: never run them */
    return len;
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
#define ICON_BASE_LBA        1261
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
