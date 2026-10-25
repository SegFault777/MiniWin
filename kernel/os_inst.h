/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef OS_INST_H
#define OS_INST_H
#include "storage.h"
#include "fs.h"
#include "chain_mbr.h"

/* os_inst.h -- copies the running MiniWin onto another storage area (the terminal's `os-inst <id> [<arch>]`).
 *
 * WHAT GETS WRITTEN. One complete installation = the layout of fs.h, starting at the target's first sector (the whole
 * device for "<type><n>00", or the partition's first sector):
 *     LBA 0          the boot sector (boot/boot.asm) with its install fields patched: where this installation starts,
 *                    and a fresh random install id (so the kernel can find its own disk again at boot)
 *     LBA 1..1028    stage 2 + the kernel                     } copied from the disk MiniWin booted from
 *     LBA 1029..1732 the icon catalog                         }
 *     LBA 1733..     a FRESH, EMPTY root filesystem (MWFS), formatted over the rest of the target (up to 8 GB of it; a
 *                    bigger partition keeps its tail for `part format` + `part mount`). A new system starts with NO
 *                    user data, and stale bytes in the target can never be mistaken for a file.
 * <arch> is the processor architecture the installation is for. MiniWin is a 32-bit x86 system, so the only values with
 * a meaning today are x86 / i386 / ia32 / i686 / 32; anything else is refused with a clear message instead of writing an
 * installation that could not start.
 *
 * HOW IT STAYS SAFE.
 *   - The target was already vetted by st_resolve() (exists, real partition, big enough, not where MiniWin is
 *     running). The terminal also makes the user confirm before this ever starts.
 *   - Sector 0 -- the one that makes the area bootable -- is written LAST. An install that fails or is interrupted
 *     part-way therefore never looks bootable, and when installing to a whole device the old partition table
 *     survives until the very last write.
 *   - Everything is read back and compared before the install is declared successful.
 *   - The disk's own MBR boot code is never overwritten: only if the MBR code area (bytes 0-445) is entirely blank
 *     does os-inst add a minimal "boot the active partition" loader (and mark the target partition active). See
 *     INST_MBR_*.
 *
 * It is a state machine advanced a few sectors per main-loop pass (inst_step()), so the UI keeps drawing and the
 * terminal can print progress while ~6000 sector operations go by. */

#define INST_PHASE_IDLE      0
#define INST_PHASE_WRITE     1
#define INST_PHASE_VERIFY    2
#define INST_PHASE_BOOTCODE  3
#define INST_PHASE_DONE      4
#define INST_PHASE_FAILED    5

#define INST_ERR_NONE         0
#define INST_ERR_READ_SOURCE  1   /* could not read MiniWin's own disk */
#define INST_ERR_WRITE        2   /* the target refused a sector */
#define INST_ERR_READ_TARGET  3   /* could not read the target back */
#define INST_ERR_VERIFY       4   /* what is on the target is not what was written */
#define INST_ERR_FORMAT       5   /* the root filesystem could not be written or did not read back */

/* What happened to the DISK's boot sector (relevant for partition installs only) */
#define INST_MBR_NONE         0   /* whole-device install: sector 0 is MiniWin's own boot sector, nothing to chain */
#define INST_MBR_CHAIN        1   /* blank MBR code area: chain loader written, the partition marked active */
#define INST_MBR_KEPT         2   /* the disk already has boot code: left untouched */
#define INST_MBR_UNSUPPORTED  3   /* blank, but this partition cannot be started by the chain loader (logical / GPT) */

typedef struct {
    int phase;
    st_target_t t;
    u32 id;               /* the install id patched into the new boot sector */
    u32 next;             /* position in the current phase (0 .. INST_COPY_SECTORS) */
    int formatted;        /* the root filesystem has been written */
    u32 root_sectors;     /* how big a root volume this installation gets */
    int fmt_err;          /* mwfs error from the format, if any */
    int err;
    u32 err_lba;          /* the installation-relative sector the error is about */
    int mbr_result;
} inst_job_t;
static inst_job_t inst;

/* A test hook: when non-zero the next job uses this id instead of a fresh one. */
static u32 inst_id_override = 0;

/* Is this installation-relative sector copied from the running system (vs. left zero)? */
static inline int inst_is_system_sector(u32 lba) { return lba < MW_ROOTFS_LBA; }     /* boot sector, stage 2, kernel, icon catalog */

/* The sectors that are COPIED, in the order they are written: 1, 2, ... 1732, and then 0 (see the safety notes above). The
 * root filesystem (LBA 1733 and up) is not copied but formatted, just before sector 0. */
#define INST_COPY_SECTORS MW_ROOTFS_LBA
static inline u32 inst_lba_for(u32 k) { return (k + 1) % INST_COPY_SECTORS; }

/* The architecture word of `os-inst <id> <arch>`: 1 = 32-bit x86 (what this kernel is), 0 = known but not buildable here,
 * -1 = not an architecture name. Case-insensitive. */
static inline int inst_arch_check(const char *a) {
    static const char *const ok[] = { "x86", "i386", "i486", "i586", "i686", "ia32", "32" };
    static const char *const other[] = { "x64", "x86_64", "x86-64", "amd64", "arm", "arm64", "aarch64", "riscv", "riscv64", "ppc", "mips", "64" };
    for (unsigned i = 0; i < sizeof ok / sizeof *ok; i++) if (mwfs_name_eq(a, ok[i])) return 1;
    for (unsigned i = 0; i < sizeof other / sizeof *other; i++) if (mwfs_name_eq(a, other[i])) return 0;
    return -1;
}

/* A fresh, non-zero id that differs from the running system's. TSC + ticks-ish noise + the previous id through an
 * xorshift: unique enough to tell two installations apart (it is an identifier, not a secret). */
static inline u32 inst_new_id(u32 running_id) {
    if (inst_id_override) return inst_id_override;
    u32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    u32 x = lo ^ (hi * 0x9E3779B1u) ^ (running_id * 1664525u + 1013904223u);
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    if (x == 0 || x == running_id) x = 0xA5C3E11Du ^ running_id;
    if (x == 0) x = 1;
    return x;
}

/* What sector `lba` of the new installation must contain. Returns 0 if MiniWin's own disk could not be read. */
static inline int inst_expected(u32 lba, u8 *out) {
    if (inst_is_system_sector(lba)) {
        for (int i = 0; i < 512; i++) out[i] = 0;
        if (!ata_read_sector(lba, out)) return 0;                         /* the boot device, relative to MiniWin's own base */
    } else {
        for (int i = 0; i < 512; i++) out[i] = 0;
    }
    if (lba == 0) {
        *(u32 *)(out + ST_BOOT_FIELDS_OFF)     = inst.t.base;             /* install_base */
        *(u32 *)(out + ST_BOOT_FIELDS_OFF + 4) = inst.id;                 /* install_id */
    }
    return 1;
}

/* Starts a job for a target that st_resolve() accepted. */
static inline void inst_begin(const st_target_t *t, u32 running_id) {
    inst.t = *t;
    inst.id = inst_new_id(running_id);
    inst.phase = INST_PHASE_WRITE;
    inst.next = 0; inst.err = INST_ERR_NONE; inst.err_lba = 0; inst.mbr_result = INST_MBR_NONE;
    inst.formatted = 0; inst.fmt_err = 0;
    u32 room = t->sectors > MW_ROOTFS_LBA ? t->sectors - MW_ROOTFS_LBA : 0;
    inst.root_sectors = room > 16u * 1024u * 1024u ? 16u * 1024u * 1024u : room;       /* 8 GB: the bitmap walk of `part check` stays quick */
}

static inline void inst_fail(int err, u32 lba) { inst.phase = INST_PHASE_FAILED; inst.err = err; inst.err_lba = lba; }

static inline int inst_target_write(u32 lba, const u8 *buf) {
    return blk_write(inst.t.pos, inst.t.base + lba, buf);
}
static inline int inst_target_read(u32 lba, u8 *buf) {
    for (int i = 0; i < 512; i++) buf[i] = 0;
    return blk_read(inst.t.pos, inst.t.base + lba, buf);
}

/* After a PARTITION install: should the disk's own sector 0 learn how to start it? (see the INST_MBR_* notes) */
static inline void inst_bootcode(void) {
    if (inst.t.whole) { inst.mbr_result = INST_MBR_NONE; return; }
    u8 s[512];
    for (int i = 0; i < 512; i++) s[i] = 0;
    if (!blk_read(inst.t.pos, 0, s)) { inst.mbr_result = INST_MBR_KEPT; return; }   /* cannot even look: touch nothing */

    int blank = 1;
    for (int i = 0; i < CHAIN_MBR_SIZE; i++) if (s[i]) { blank = 0; break; }
    if (!blank) { inst.mbr_result = INST_MBR_KEPT; return; }                // somebody's boot loader lives here: hands off

    /* The chain loader boots the first ACTIVE PRIMARY partition of a classic MBR table. Anything else (a logical
     * partition, a GPT disk, a missing table) cannot be started this way. */
    if (st_scheme(inst.t.pos) != ST_SCHEME_MBR || inst.t.partition.is_gpt || inst.t.part < 1 || inst.t.part > 4) {
        inst.mbr_result = INST_MBR_UNSUPPORTED;
        return;
    }
    for (int i = 0; i < CHAIN_MBR_SIZE; i++) s[i] = chain_mbr_code[i];
    for (int e = 0; e < 4; e++) s[446 + e * 16] = (e == (int)inst.t.part - 1) ? 0x80 : 0x00;   // exactly one active partition: ours
    s[510] = 0x55; s[511] = 0xAA;
    if (!blk_write(inst.t.pos, 0, s)) { inst.mbr_result = INST_MBR_KEPT; return; }
    u8 back[512];
    for (int i = 0; i < 512; i++) back[i] = 0;
    if (!blk_read(inst.t.pos, 0, back)) { inst.mbr_result = INST_MBR_KEPT; return; }
    for (int i = 0; i < 512; i++) if (back[i] != s[i]) { inst.mbr_result = INST_MBR_KEPT; return; }
    inst.mbr_result = INST_MBR_CHAIN;
}

/* Advances the job by up to `budget` sectors. Returns the phase afterwards. */
static inline int inst_step(u32 budget) {
    u8 want[512], got[512];
    while (budget-- > 0) {
        if (inst.phase == INST_PHASE_WRITE) {
            u32 lba = inst_lba_for(inst.next);
            if (lba == 0 && !inst.formatted) {                            /* everything but the boot sector is down: now the filesystem */
                mwfs_vol_t v;
                mwfs_zero(&v, sizeof v);
                v.dev = inst.t.pos; v.base = inst.t.base + MW_ROOTFS_LBA; v.sectors = inst.root_sectors;
                int e = mwfs_mkfs(&v, "MINIWIN");
                if (e == MWFS_OK) e = vfs_make_root_layout(&v);
                inst.fmt_err = e;
                if (e != MWFS_OK) { inst_fail(INST_ERR_FORMAT, MW_ROOTFS_LBA); return inst.phase; }
                inst.formatted = 1;
            }
            if (!inst_expected(lba, want)) { inst_fail(INST_ERR_READ_SOURCE, lba); return inst.phase; }
            if (!inst_target_write(lba, want)) { inst_fail(INST_ERR_WRITE, lba); return inst.phase; }
            if (++inst.next >= INST_COPY_SECTORS) { inst.phase = INST_PHASE_VERIFY; inst.next = 0; }
        } else if (inst.phase == INST_PHASE_VERIFY) {
            u32 lba = inst_lba_for(inst.next);
            if (!inst_expected(lba, want)) { inst_fail(INST_ERR_READ_SOURCE, lba); return inst.phase; }
            if (!inst_target_read(lba, got)) { inst_fail(INST_ERR_READ_TARGET, lba); return inst.phase; }
            for (int i = 0; i < 512; i++) if (want[i] != got[i]) { inst_fail(INST_ERR_VERIFY, lba); return inst.phase; }
            if (++inst.next >= INST_COPY_SECTORS) {
                mwfs_vol_t v;                                              /* and the new root filesystem must open and be clean-looking */
                mwfs_zero(&v, sizeof v);
                v.dev = inst.t.pos; v.base = inst.t.base + MW_ROOTFS_LBA; v.sectors = inst.root_sectors;
                if (mwfs_open(&v) != MWFS_OK) { inst_fail(INST_ERR_FORMAT, MW_ROOTFS_LBA); return inst.phase; }
                inst.phase = INST_PHASE_BOOTCODE; inst.next = 0;
            }
        } else if (inst.phase == INST_PHASE_BOOTCODE) {
            inst_bootcode();
            inst.phase = INST_PHASE_DONE;
        } else {
            break;
        }
    }
    return inst.phase;
}

static inline int inst_active(void) { return inst.phase >= INST_PHASE_WRITE && inst.phase <= INST_PHASE_BOOTCODE; }

/* 0..100 for a progress display: writing is the first half, verifying the second. */
static inline u32 inst_percent(void) {
    if (inst.phase == INST_PHASE_WRITE)   return inst.next * 50 / INST_COPY_SECTORS;
    if (inst.phase == INST_PHASE_VERIFY)  return 50 + inst.next * 50 / INST_COPY_SECTORS;
    if (inst.phase == INST_PHASE_BOOTCODE || inst.phase == INST_PHASE_DONE) return 100;
    return 0;
}

static inline const char *inst_error_text(int e) {
    switch (e) {
        case INST_ERR_READ_SOURCE: return "could not read MiniWin's own disk";
        case INST_ERR_WRITE:       return "the target refused a write";
        case INST_ERR_READ_TARGET: return "could not read the target back";
        case INST_ERR_VERIFY:      return "what is on the target is not what was written";
        case INST_ERR_FORMAT:      return "the new filesystem could not be written to the target";
        default:                   return "";
    }
}

static inline const char *inst_mbr_text(int m) {
    switch (m) {
        case INST_MBR_CHAIN:       return "the disk had no boot code: a loader for the active partition was added and this partition was marked active";
        case INST_MBR_KEPT:        return "the disk's own boot code was left alone: point your boot manager at this partition to start MiniWin";
        case INST_MBR_UNSUPPORTED: return "this partition cannot be started by the built-in loader (logical or GPT): use a boot manager that can chainload it";
        default:                   return "";
    }
}

/* ---- what is on the target before we overwrite it (for the confirmation prompt) ---- */
typedef struct {
    int readable;
    int non_empty;        /* any non-zero byte in the first sector of the region */
    int has_mbr_sig;      /* 0x55 0xAA at the end of that sector (a boot sector or a partition table) */
    int partitions;       /* whole-device target: how many partitions are about to be destroyed */
    int miniwin;          /* the first sector carries an install id: this looks like an existing MiniWin */
} inst_probe_t;

static inline void inst_probe(const st_target_t *t, inst_probe_t *out) {
    u8 s[512];
    out->readable = out->non_empty = out->has_mbr_sig = out->partitions = out->miniwin = 0;
    for (int i = 0; i < 512; i++) s[i] = 0;
    if (!blk_read(t->pos, t->base, s)) return;
    out->readable = 1;
    for (int i = 0; i < 512; i++) if (s[i]) { out->non_empty = 1; break; }
    out->has_mbr_sig = (s[510] == 0x55 && s[511] == 0xAA);
    out->miniwin = st_le32(s + ST_BOOT_FIELDS_OFF + 4) != 0 && out->has_mbr_sig;
    if (t->whole) { st_part_t list[8]; out->partitions = st_list_partitions(t->pos, list, 8); }
}

#endif
