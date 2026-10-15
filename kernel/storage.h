/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef STORAGE_H
#define STORAGE_H
#include "ata.h"

/* storage.h -- which storage devices exist, how they are partitioned, and the "storage id" that names one of
 * their partitions (the argument of the terminal's `os-inst`). Nothing here WRITES to a disk; installing is
 * os_inst.h's job, and it leans on this file to decide, safely, WHERE.
 *
 * ---- Storage ids ----------------------------------------------------------------------------------------------
 *      <type><device number><partition number, always 2 digits>
 *   type    one of ide / sata / nvme / scsi / usb
 *   device  1-based, counted within its type (any number of digits)
 *   part    01, 02, 03 ... = the partition with that number; 00 = the WHOLE device (no partition table involved)
 *   examples: "sata103" = the 1st SATA device, partition 3.   "ide200" = the whole 2nd IDE device.
 *   The last two digits are always the partition, so "nvme1205" is device 12 partition 05 -- a one-digit
 *   device number is just the common case.
 *
 * ---- What this build can actually reach ------------------------------------------------------------------------
 *   Only `ide` has a driver: the legacy ATA ports, four positions numbered by where the disk is plugged in
 *   (ide1 = primary master, ide2 = primary slave, ide3 = secondary master, ide4 = secondary slave -- numbered by
 *   POSITION, not by order of appearance, so an id keeps meaning the same drive when another one is added).
 *   The other type names parse (so an id like "nvme101" gets a precise answer instead of "syntax error"),
 *   and are reported as "no driver in this build". They are the next drivers to write, not an oversight.
 *
 * ---- Partition numbers -----------------------------------------------------------------------------------------
 *   MBR: 01-04 = the four primary entries (by table position), 05, 06 ... = logical partitions inside the
 *        extended partition, in chain order (the same convention Linux uses). The extended container itself is
 *        listed but is not a place to install.
 *   GPT: the partition's entry number (entries 1 and 3 in use -> partitions 01 and 03). Detected through the
 *        protective MBR (type 0xEE) plus a valid "EFI PART" header; only 32-bit LBAs are supported.
 */

typedef enum { ST_IDE = 0, ST_SATA, ST_NVME, ST_SCSI, ST_USB, ST_TYPE_COUNT } st_type_t;
static const char *const st_type_names[ST_TYPE_COUNT] = { "ide", "sata", "nvme", "scsi", "usb" };
static inline int st_type_has_driver(st_type_t t) { return t == ST_IDE; }

/* result codes of st_parse_id() / st_resolve() */
#define ST_OK              0
#define ST_E_SYNTAX        1   /* not <type><digits> with at least device + 2-digit partition */
#define ST_E_UNKNOWN_TYPE  2
#define ST_E_NO_DRIVER     3   /* a known type this build has no driver for */
#define ST_E_NO_DEVICE     4
#define ST_E_NO_PARTITION  5
#define ST_E_EXTENDED      6   /* the partition is an MBR extended container: it holds partitions, it is not one */
#define ST_E_TOO_SMALL     7
#define ST_E_OVERLAP_BOOT  8   /* the target overlaps the installation that is running right now */
#define ST_E_RANGE         9   /* the partition table points outside the device */

/* The size of one complete MiniWin installation (the fixed disk layout of fs.h: LBA 0..2047). */
#define ST_INSTALL_SECTORS 2048

/* ================================================================ devices */
typedef struct {
    int  present;
    u32  sectors;       /* usable (LBA28-addressable) sectors */
    char model[41];
} st_dev_t;
static st_dev_t st_devs[ATA_POSITIONS];
static int st_scanned = 0;

/* Probes the four IDE positions. Cheap when positions are empty (status reads 0x00/0xFF). */
static inline void st_scan(void) {
    for (int i = 0; i < ATA_POSITIONS; i++) {
        ata_ident_t id;
        st_devs[i].present = 0; st_devs[i].sectors = 0; st_devs[i].model[0] = 0;
        if (ata_at_identify(&ata_positions[i], &id)) {
            st_devs[i].present = 1;
            st_devs[i].sectors = id.sectors;
            for (int k = 0; k < 41; k++) st_devs[i].model[k] = id.model[k];
        }
    }
    st_scanned = 1;
}

/* ================================================================ partitions */
typedef struct {
    int number;            /* 1-based, as in the storage id; 0 is never a partition (it means "whole device") */
    u32 start;             /* first absolute sector */
    u32 sectors;
    u8  type;              /* MBR partition type byte (0 for GPT entries) */
    int is_gpt;
    int active;            /* MBR "bootable" flag */
    int extended_container;/* MBR extended partition: contains logical partitions, is not itself installable */
} st_part_t;

#define ST_SCHEME_NONE 0   /* no valid partition table (blank, or a table-less volume) */
#define ST_SCHEME_MBR  1
#define ST_SCHEME_GPT  2

#define ST_MAX_PART_NUMBER 99
#define ST_MAX_LOGICAL     32   /* logical-partition chain walk limit (a corrupt or cyclic chain must still end) */

static inline u32 st_le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static inline int st_is_extended_type(u8 t) { return t == 0x05 || t == 0x0F || t == 0x85; }

/* One 16-byte MBR entry -> st_part_t. Returns 0 for an empty entry. */
static inline int st_mbr_entry(const u8 *e, int number, st_part_t *out) {
    u8 type = e[4];
    u32 start = st_le32(e + 8), count = st_le32(e + 12);
    if (type == 0 || count == 0) return 0;
    out->number = number; out->start = start; out->sectors = count; out->type = type;
    out->is_gpt = 0; out->active = (e[0] == 0x80); out->extended_container = st_is_extended_type(type);
    return 1;
}

static inline int st_read(int pos, u32 lba, u8 *buf) {
    for (int i = 0; i < 512; i++) buf[i] = 0;
    return ata_at_read(ata_positions[pos].io, ata_positions[pos].slave, lba, buf);
}

/* A valid GPT header at LBA 1? (and fills the geometry we need) */
static inline int st_gpt_header(int pos, u32 *entries_lba, u32 *count, u32 *esize) {
    u8 h[512];
    if (!st_read(pos, 1, h)) return 0;
    static const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
    for (int i = 0; i < 8; i++) if (h[i] != (u8)sig[i]) return 0;
    if (st_le32(h + 76) != 0) return 0;                                   /* the entries LBA is a u64: its high half must be 0 */
    *entries_lba = st_le32(h + 72);
    *count = st_le32(h + 80);
    *esize = st_le32(h + 84);
    if (*esize < 128 || *esize > 512 || (512 % *esize) != 0) return 0;
    if (*count == 0) return 0;
    if (*count > 128) *count = 128;
    return 1;
}

/* Which partitioning scheme does the device carry? */
static inline int st_scheme(int pos) {
    u8 s[512];
    if (!st_read(pos, 0, s)) return ST_SCHEME_NONE;
    if (s[510] != 0x55 || s[511] != 0xAA) return ST_SCHEME_NONE;
    int protective = 0, any = 0;
    for (int i = 0; i < 4; i++) { u8 t = s[446 + i * 16 + 4]; if (t == 0xEE) protective = 1; if (t) any = 1; }
    if (protective) {
        u32 a, c, e;
        if (st_gpt_header(pos, &a, &c, &e)) return ST_SCHEME_GPT;
    }
    return any ? ST_SCHEME_MBR : ST_SCHEME_NONE;
}

/* Fills `out` with partition `number` of the device at `pos`. Returns 1 if it exists. */
static inline int st_get_partition(int pos, int number, st_part_t *out) {
    if (number < 1 || number > ST_MAX_PART_NUMBER) return 0;
    int scheme = st_scheme(pos);
    u8 s[512];

    if (scheme == ST_SCHEME_GPT) {
        u32 elba, count, esize;
        if (!st_gpt_header(pos, &elba, &count, &esize)) return 0;
        if ((u32)number > count) return 0;
        u32 byte_off = (u32)(number - 1) * esize;
        if (!st_read(pos, elba + byte_off / 512, s)) return 0;
        const u8 *e = s + (byte_off % 512);
        int empty = 1;
        for (int i = 0; i < 16; i++) if (e[i]) empty = 0;
        if (empty) return 0;
        if (st_le32(e + 36) != 0 || st_le32(e + 44) != 0) return 0;      /* 64-bit LBAs: out of range for this driver */
        u32 first = st_le32(e + 32), last = st_le32(e + 40);
        if (last < first) return 0;
        out->number = number; out->start = first; out->sectors = last - first + 1; out->type = 0;
        out->is_gpt = 1; out->active = 0; out->extended_container = 0;
        return 1;
    }
    if (scheme != ST_SCHEME_MBR) return 0;
    if (!st_read(pos, 0, s)) return 0;

    if (number <= 4) return st_mbr_entry(s + 446 + (number - 1) * 16, number, out);

    /* logical partitions: walk the EBR chain of the first extended container */
    u32 ext_start = 0; int have_ext = 0;
    for (int i = 0; i < 4 && !have_ext; i++) {
        st_part_t p;
        if (st_mbr_entry(s + 446 + i * 16, i + 1, &p) && p.extended_container) { ext_start = p.start; have_ext = 1; }
    }
    if (!have_ext) return 0;
    u32 cur = ext_start;
    for (int n = 5; n < 5 + ST_MAX_LOGICAL; n++) {
        u8 e[512];
        if (!st_read(pos, cur, e)) return 0;
        if (e[510] != 0x55 || e[511] != 0xAA) return 0;
        if (n == number) {
            st_part_t p;
            if (!st_mbr_entry(e + 446, n, &p)) return 0;
            p.start = cur + p.start;                                      /* relative to this EBR */
            p.extended_container = 0;
            *out = p;
            return 1;
        }
        if (!st_is_extended_type(e[446 + 16 + 4]) || st_le32(e + 446 + 16 + 12) == 0) return 0;   /* end of chain */
        u32 next = ext_start + st_le32(e + 446 + 16 + 8);
        if (next <= cur) return 0;                                         /* chains only move forward */
        cur = next;
    }
    return 0;
}

/* Lists the device's partitions in number order. Returns how many were stored (at most `max`). */
static inline int st_list_partitions(int pos, st_part_t *out, int max) {
    int n = 0, scheme = st_scheme(pos);
    if (scheme == ST_SCHEME_NONE) return 0;
    int limit = scheme == ST_SCHEME_GPT ? 128 : 4 + ST_MAX_LOGICAL;
    for (int num = 1; num <= limit && num <= ST_MAX_PART_NUMBER && n < max; num++) {
        st_part_t p;
        if (st_get_partition(pos, num, &p)) out[n++] = p;
        else if (scheme == ST_SCHEME_MBR && num > 4) break;                /* the logical chain has ended */
    }
    return n;
}

/* ================================================================ storage ids */
typedef struct { st_type_t type; u32 dev; u32 part; } st_id_t;

static inline int st_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Syntax only: "<type><dev digits><2 partition digits>". Case-insensitive. */
static inline int st_parse_id(const char *s, st_id_t *out) {
    char word[8]; int wl = 0;
    int i = 0;
    while (s[i] && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) {
        if (wl < 7) word[wl++] = (char)st_lower(s[i]);
        i++;
    }
    word[wl] = 0;
    if (wl == 0) return ST_E_SYNTAX;
    int digits_start = i, nd = 0;
    while (s[i] >= '0' && s[i] <= '9') { i++; nd++; }
    if (s[i] != 0) return ST_E_SYNTAX;                                     /* stray characters */
    int t = -1;
    for (int k = 0; k < ST_TYPE_COUNT; k++) {
        const char *n = st_type_names[k]; int j = 0;
        while (n[j] && word[j] == n[j]) j++;
        if (!n[j] && !word[j]) { t = k; break; }
    }
    if (t < 0) return ST_E_UNKNOWN_TYPE;
    if (nd < 3 || nd > 8) return ST_E_SYNTAX;                              /* at least 1 device digit + 2 partition digits */
    u32 dev = 0;
    for (int k = digits_start; k < i - 2; k++) dev = dev * 10 + (u32)(s[k] - '0');
    u32 part = (u32)(s[i - 2] - '0') * 10 + (u32)(s[i - 1] - '0');
    if (dev == 0) return ST_E_SYNTAX;                                      /* devices are numbered from 1 */
    out->type = (st_type_t)t; out->dev = dev; out->part = part;
    return ST_OK;
}

/* Writes the canonical id ("sata103") into out (>= 16 bytes). */
static inline void st_format_id(st_type_t type, u32 dev, u32 part, char *out) {
    int n = 0;
    for (const char *p = st_type_names[type]; *p; p++) out[n++] = *p;
    char tmp[12]; int tn = 0;
    do { tmp[tn++] = (char)('0' + dev % 10); dev /= 10; } while (dev && tn < 10);
    while (tn) out[n++] = tmp[--tn];
    out[n++] = (char)('0' + (part / 10) % 10);
    out[n++] = (char)('0' + part % 10);
    out[n] = 0;
}

/* What an id points at, once resolved. */
typedef struct {
    st_type_t type;
    u32 dev;               /* the device number from the id */
    u32 part;              /* the partition number from the id (0 = whole device) */
    int whole;
    int pos;               /* ATA position (0..3), valid for ST_IDE */
    u32 base;              /* absolute first sector of the target region */
    u32 sectors;           /* its length */
    st_part_t partition;   /* valid when !whole */
    char id[16];
} st_target_t;

/* Do two sector ranges overlap? */
static inline int st_overlap(u32 a, u32 alen, u32 b, u32 blen) {
    return a < b + blen && b < a + alen;
}

/* Turns an id string into a concrete, SAFE target, or says why not (ST_E_*). Safe means: the device exists and the
 * region lies inside it, a partition is a real (non-container) partition, it is big enough for a complete
 * installation, and none of it overlaps the installation this kernel is running from. */
static inline int st_resolve(const char *s, st_target_t *t) {
    st_id_t id;
    int r = st_parse_id(s, &id);
    if (r != ST_OK) return r;
    t->type = id.type; t->dev = id.dev; t->part = id.part; t->whole = (id.part == 0);
    st_format_id(id.type, id.dev, id.part, t->id);
    if (!st_type_has_driver(id.type)) return ST_E_NO_DRIVER;

    if (!st_scanned) st_scan();
    if (id.dev < 1 || id.dev > ATA_POSITIONS) return ST_E_NO_DEVICE;
    t->pos = (int)id.dev - 1;
    if (!st_devs[t->pos].present) return ST_E_NO_DEVICE;
    u32 dev_sectors = st_devs[t->pos].sectors;

    if (t->whole) {
        t->base = 0; t->sectors = dev_sectors;
    } else {
        if (!st_get_partition(t->pos, (int)id.part, &t->partition)) return ST_E_NO_PARTITION;
        if (t->partition.extended_container) return ST_E_EXTENDED;
        t->base = t->partition.start; t->sectors = t->partition.sectors;
        if (t->base >= dev_sectors || t->sectors > dev_sectors - t->base) return ST_E_RANGE;
    }
    if (t->sectors < ST_INSTALL_SECTORS) return ST_E_TOO_SMALL;
    /* The WHOLE target region, not just the 2048 sectors an install will write, is compared with the running
     * installation's footprint: installing to a whole device ("00") rewrites sector 0 and with it the partition table,
     * which would orphan a MiniWin that is running from a partition inside that very device. */
    if (t->pos == ata_boot_pos && st_overlap(t->base, t->sectors, ata_boot_base, ST_INSTALL_SECTORS)) return ST_E_OVERLAP_BOOT;
    return ST_OK;
}

static inline const char *st_error_text(int e) {
    switch (e) {
        case ST_E_SYNTAX:       return "bad id: expected <type><device><2-digit partition>, e.g. ide101 (see os-inst ?)";
        case ST_E_UNKNOWN_TYPE: return "unknown storage type (known: ide sata nvme scsi usb)";
        case ST_E_NO_DRIVER:    return "this build has no driver for that storage type yet (only ide)";
        case ST_E_NO_DEVICE:    return "no such device (see os-inst list)";
        case ST_E_NO_PARTITION: return "no such partition on that device (see os-inst list)";
        case ST_E_EXTENDED:     return "that is an extended-partition container; pick a logical partition inside it";
        case ST_E_TOO_SMALL:    return "target is smaller than 1 MB (2048 sectors), too small for MiniWin";
        case ST_E_OVERLAP_BOOT: return "that area is where MiniWin is running from right now";
        case ST_E_RANGE:        return "the partition table points outside the device";
        default:                return "ok";
    }
}

/* ================================================================ finding the boot device */
/* The installation fields os-inst patches into the boot sector (boot/boot.asm: INSTALL_FIELDS_OFF). */
#define ST_BOOT_FIELDS_OFF 432

/* The boot loader tells the kernel two numbers (see boot.asm): where this installation starts on its disk and its
 * unique install id (0 = a stock image that was never installed by os-inst: it owns the whole of whichever disk it
 * booted from, which this kernel has always taken to be the primary master). For an installed system the disk may
 * be ANY of the four positions, so find the one whose boot sector carries our id at our base. */
static u32 st_boot_id = 0;     /* the running installation's id (0 for a stock image) -- os-inst makes sure a copy gets a different one */

static inline void st_boot_locate(u32 base, u32 id) {
    ata_boot_pos = 0; ata_boot_base = base; st_boot_id = id;
    if (id == 0) return;
    for (int i = 0; i < ATA_POSITIONS; i++) {
        u8 s[512];
        ata_ident_t ident;
        if (!ata_at_identify(&ata_positions[i], &ident)) continue;
        if (!st_read(i, base, s)) continue;
        if (st_le32(s + ST_BOOT_FIELDS_OFF) == base && st_le32(s + ST_BOOT_FIELDS_OFF + 4) == id) {
            ata_boot_pos = i;
            return;
        }
    }
}

#endif
