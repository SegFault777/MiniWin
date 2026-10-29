/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SHELL_PART_H
#define SHELL_PART_H

/* shell_part.h -- MiniShell's `part` command: look at, format, mount, check and (on MBR disks) edit the partitions of the
 * storage devices this kernel drives, and the `usb` listing.
 *
 *   part list                     every disk, its partitions, what filesystem is on them
 *   part info <id>                one partition or whole disk in detail
 *   part format <id>              lay a fresh MWFS filesystem over it (asks YES; erases it)
 *   part mount <id>               make it appear as /mnt/<id>
 *   part unmount <id>             take /mnt/<id> away again
 *   part check <id> [fix]         verify the filesystem; with `fix`, give back what a crash left behind
 *   part create <id> [mb]         new primary MBR partition in free space (number 1-4, default: the largest gap)
 *   part delete <id>              remove an MBR partition entry (asks YES; the data is not touched but unreachable)
 *   part set <id> active|inactive|type <hex>   the bootable flag / the type byte of an MBR entry
 *   part merge <id1> <id2>        join two neighbouring MBR partitions into one (asks YES; erases both)
 *
 * <id> is the same id os-inst uses: <type><disk><2-digit partition>, "sata102", "ide200" (00 = the whole disk).
 * Writing a partition table is only ever done on MBR disks, only to primary entries 1-4, and never touches anything that
 * overlaps the installation MiniWin is running from. GPT disks and extended/logical partitions can be listed, formatted and
 * mounted, not edited. */

#define PART_NEW_TYPE   0xDA            /* "non-file-system data": what `part create` marks new partitions with */
#define PART_MAX_VOL    (16u * 1024u * 1024u)   /* sectors of a volume `part format` writes (8 GB): the rest of a bigger target stays unused */

typedef struct { int active; int op; char id1[16], id2[16]; st_target_t t1, t2; } part_pending_t;
enum { PART_OP_NONE = 0, PART_OP_FORMAT, PART_OP_DELETE, PART_OP_MERGE };
static part_pending_t part_pending;

static void part_line(const char *a, const char *b) {
    char line[TERM_COLS + 1]; u32 l = 0;
    append_str(line, &l, sizeof(line), a);
    if (b) append_str(line, &l, sizeof(line), b);
    term_print(line);
}
static void part_num_line(const char *a, u32 n, const char *b) {
    char line[TERM_COLS + 1]; u32 l = 0;
    append_str(line, &l, sizeof(line), a);
    append_uint(line, &l, sizeof(line), (int)n);
    if (b) append_str(line, &l, sizeof(line), b);
    term_print(line);
}

/* Syntax + existence of an id -> a target, WITHOUT the "big enough to install MiniWin" rule st_resolve has (a data volume
 * can be small). allow_boot = 0 refuses anything overlapping the running installation. */
static int part_resolve(const char *s, st_target_t *t, int allow_boot) {
    st_id_t id;
    int r = st_parse_id(s, &id);
    if (r != ST_OK) return r;
    t->type = id.type; t->dev = id.dev; t->part = id.part; t->whole = (id.part == 0);
    st_format_id(id.type, id.dev, id.part, t->id);
    if (!st_type_has_driver(id.type)) return ST_E_NO_DRIVER;
    if (!st_scanned) st_scan();
    t->pos = blk_index(id.type, id.dev);
    if (t->pos < 0 || !st_devs[t->pos].present) return ST_E_NO_DEVICE;
    if (id.type == ST_CD) return ST_E_READ_ONLY;
    u32 total = st_devs[t->pos].sectors;
    if (t->whole) { t->base = 0; t->sectors = total; }
    else {
        if (!st_get_partition(t->pos, (int)id.part, &t->partition)) return ST_E_NO_PARTITION;
        if (t->partition.extended_container) return ST_E_EXTENDED;
        t->base = t->partition.start; t->sectors = t->partition.sectors;
        if (t->base >= total || t->sectors > total - t->base) return ST_E_RANGE;
    }
    if (t->sectors < 64) return ST_E_TOO_SMALL;
    if (!allow_boot && t->pos == blk_boot_dev && st_overlap(t->base, t->sectors, blk_boot_base, ST_INSTALL_SECTORS)) return ST_E_OVERLAP_BOOT;
    return ST_OK;
}
static int part_resolve_say(const char *s, st_target_t *t, int allow_boot) {
    if (!s[0]) { term_print("  which partition? (part list)"); return 0; }
    int r = part_resolve(s, t, allow_boot);
    if (r != ST_OK) { term_print_wrapped(st_error_text(r), 2); return 0; }
    return 1;
}

/* Where an MWFS volume would be inside a target: at its very start (a data volume), or at MW_ROOTFS_LBA (a MiniWin
 * installation's root volume). 0 = none, 1 = data volume, 2 = installed system. */
static int part_find_volume(const st_target_t *t, u32 *vbase, u32 *vsectors) {
    mwfs_vol_t v;
    mwfs_zero(&v, sizeof v);
    v.dev = t->pos; v.base = t->base; v.sectors = t->sectors;
    int e = mwfs_open(&v);
    if (e == MWFS_OK || e == MWFS_E_CORRUPT) { *vbase = t->base; *vsectors = t->sectors; return e == MWFS_OK ? 1 : -1; }
    if (t->sectors > MW_ROOTFS_LBA + 64) {
        mwfs_zero(&v, sizeof v);
        v.dev = t->pos; v.base = t->base + MW_ROOTFS_LBA; v.sectors = t->sectors - MW_ROOTFS_LBA;
        e = mwfs_open(&v);
        if (e == MWFS_OK || e == MWFS_E_CORRUPT) { *vbase = v.base; *vsectors = t->sectors - MW_ROOTFS_LBA; return e == MWFS_OK ? 2 : -1; }
    }
    return 0;
}
static const char *part_fs_tag(const st_target_t *t) {
    u32 b, n;
    int k = part_find_volume(t, &b, &n);
    return k == 1 ? "mwfs" : k == 2 ? "mwfs (MiniWin)" : k < 0 ? "mwfs DAMAGED" : "";
}

/* ---------------------------------------------------------------- list / info */
static void part_cmd_list(void) {
    if (!st_scanned) st_scan();
    int any = 0;
    for (int d = 0; d < BLK_DEV_MAX; d++) {
        if (!st_devs[d].present) continue;
        int type = blk_type_of(d);
        u32 num = (u32)(d - blk_first_of(type)) + 1;
        char id[16], line[TERM_COLS + 1]; u32 l = 0;
        st_format_id((st_type_t)type, num, 0, id);
        any = 1;
        append_str(line, &l, sizeof(line), id);
        append_str(line, &l, sizeof(line), "  ");
        term_fmt_size(line, &l, sizeof(line), st_devs[d].sectors);
        int sch = st_scheme(d);
        append_str(line, &l, sizeof(line), sch == ST_SCHEME_GPT ? "  gpt" : sch == ST_SCHEME_MBR ? "  mbr" : "  no table");
        if (d == blk_boot_dev) append_str(line, &l, sizeof(line), "  *boot");
        term_print(line);
        st_part_t parts[16];
        int n = st_list_partitions(d, parts, 16);
        for (int i = 0; i < n; i++) {
            l = 0;
            st_format_id((st_type_t)type, num, (u32)parts[i].number, id);
            append_str(line, &l, sizeof(line), "  ");
            append_str(line, &l, sizeof(line), id);
            append_str(line, &l, sizeof(line), "  ");
            term_fmt_size(line, &l, sizeof(line), parts[i].sectors);
            if (parts[i].extended_container) append_str(line, &l, sizeof(line), "  extended");
            else {
                st_target_t t;
                if (part_resolve(id, &t, 1) == ST_OK) { const char *f = part_fs_tag(&t); if (f[0]) { append_str(line, &l, sizeof(line), "  "); append_str(line, &l, sizeof(line), f); } }
            }
            if (vfs_find_mount(id)) append_str(line, &l, sizeof(line), " [mounted]");
            term_print(line);
        }
    }
    if (!any) term_print("  no disk found");
    term_print("  part ? for what you can do with them");
}

static void part_cmd_info(const char *arg) {
    st_target_t t;
    if (!part_resolve_say(arg, &t, 1)) return;
    char line[TERM_COLS + 1]; u32 l = 0;
    append_str(line, &l, sizeof(line), t.id);
    append_str(line, &l, sizeof(line), t.whole ? "  whole disk" : "  partition");
    term_print(line);
    l = 0; append_str(line, &l, sizeof(line), "  "); append_str(line, &l, sizeof(line), st_devs[t.pos].model); term_print(line);
    part_num_line("  starts at sector ", t.base, 0);
    l = 0; append_str(line, &l, sizeof(line), "  size "); term_fmt_size(line, &l, sizeof(line), t.sectors); term_print(line);
    if (!t.whole) {
        static const char hx[] = "0123456789ABCDEF";
        char ty[3] = { hx[t.partition.type >> 4], hx[t.partition.type & 15], 0 };
        part_line("  type ", ty);
        if (t.partition.active) term_print("  bootable flag set");
    }
    if (t.pos == blk_boot_dev && st_overlap(t.base, t.sectors, blk_boot_base, ST_INSTALL_SECTORS)) term_print("  MiniWin is running from here");
    u32 vb, vn;
    int k = part_find_volume(&t, &vb, &vn);
    if (k == 0) { term_print("  no MiniWin filesystem"); return; }
    mwfs_vol_t v;
    mwfs_zero(&v, sizeof v);
    v.dev = t.pos; v.base = vb; v.sectors = vn;
    if (mwfs_open(&v) != MWFS_OK) { term_print("  MWFS found but damaged"); term_print("  (part check <id>)"); return; }
    part_line(k == 2 ? "  filesystem: mwfs, a MiniWin install" : "  filesystem: mwfs", 0);
    l = 0; append_str(line, &l, sizeof(line), "  label ");
    append_str(line, &l, sizeof(line), v.label[0] ? v.label : "(none)");
    term_print(line);
    u32 free_s = mwfs_free_count(&v);
    l = 0; append_str(line, &l, sizeof(line), "  free "); term_fmt_size(line, &l, sizeof(line), free_s);
    append_str(line, &l, sizeof(line), " of "); term_fmt_size(line, &l, sizeof(line), v.sectors); term_print(line);
    if (vfs_find_mount(t.id)) part_line("  mounted at /mnt/", t.id);
}

/* ---------------------------------------------------------------- mount / unmount / format / check */
static void part_cmd_mount(const char *arg) {
    st_target_t t;
    if (!part_resolve_say(arg, &t, 0)) return;
    if (!vfs_is_root_ok()) { term_print("  no root filesystem is mounted"); return; }
    u32 vb, vn;
    int k = part_find_volume(&t, &vb, &vn);
    if (k == 0) { term_print("  no MiniWin filesystem there"); term_print("  (part format <id> makes one)"); return; }
    int e = vfs_mount_region(t.id, t.pos, vb, vn, 0);
    if (e != MWFS_OK) { term_fs_err(t.id, e); return; }
    part_line("  mounted at /mnt/", t.id);
}
static void part_cmd_unmount(const char *arg) {
    if (!arg[0]) { term_print("  which one? (ls /mnt)"); return; }
    int e = vfs_unmount(arg);
    if (e != MWFS_OK) { term_fs_err(arg, e); return; }
    term_print("  unmounted");
}
static void part_cmd_check(const char *arg, const char *mode) {
    st_target_t t;
    if (!part_resolve_say(arg, &t, 1)) return;
    int fix = mode[0] && term_streq(mode, "fix");
    if (mode[0] && !fix) { term_print("  usage: part check <id> [fix]"); return; }
    u32 vb, vn;
    int k = part_find_volume(&t, &vb, &vn);
    if (k == 0) { term_print("  no MiniWin filesystem there"); return; }
    mwfs_vol_t v;
    mwfs_zero(&v, sizeof v);
    v.dev = t.pos; v.base = vb; v.sectors = vn;
    int e = mwfs_open(&v);
    if (e != MWFS_OK) { term_fs_err("superblock", e); term_print("  (nothing to repair it from: reformat)"); return; }
    if (fix && t.pos == blk_boot_dev && st_overlap(t.base, t.sectors, blk_boot_base, ST_INSTALL_SECTORS) && vfs_is_root_ok() && vfs_vols[0].base == vb) {
        term_print("  the running root cannot be repaired");
        term_print_wrapped("while it is in use: check it without fix", 2);
        fix = 0;
    }
    term_print(fix ? "  checking and repairing..." : "  checking...");
    mwfs_report_t r;
    e = mwfs_check(&v, fix, &r);
    if (e != MWFS_OK) { term_fs_err("check", e); return; }
    part_num_line("  files ", r.files, 0);
    part_num_line("  directories ", r.dirs, 0);
    if (r.incomplete) { term_print("  directory tree too wide to check fully"); return; }
    u32 bad = r.bad_entries + r.bad_blocks + r.unmarked + r.damaged_files;
    u32 junk = r.orphans + r.leaked + r.dup_links;
    if (r.bad_entries) part_num_line("  broken directory entries: ", r.bad_entries, 0);
    if (r.bad_blocks) part_num_line("  sectors outside the volume: ", r.bad_blocks, 0);
    if (r.unmarked) part_num_line("  sectors in use but not marked: ", r.unmarked, 0);
    if (r.damaged_files) part_num_line("  files that fail their CRC: ", r.damaged_files, 0);
    if (r.orphans) part_num_line("  orphan inodes: ", r.orphans, 0);
    if (r.dup_links) part_num_line("  duplicate names: ", r.dup_links, 0);
    if (r.leaked) part_num_line("  lost sectors: ", r.leaked, 0);
    if (r.skipped_leak_scan) term_print("  (lost-sector scan skipped: > 8 GB)");
    if (!bad && !junk) term_print("  clean");
    else if (fix) part_num_line("  repaired ", r.fixed, " problem(s)");
    else if (junk && !bad) term_print_wrapped("only leftovers of a crash: part check <id> fix", 2);
    else term_print("  damage found: part check <id> fix");
    if (r.damaged_files) term_print_wrapped("(damaged files cannot be repaired: rm them)", 2);
}

static void part_confirm(int op, const char *what, const st_target_t *t1, const st_target_t *t2) {
    part_pending.active = 1; part_pending.op = op; part_pending.t1 = *t1;
    if (t2) part_pending.t2 = *t2;
    (void)what;
    term_print("Type YES to do it; else cancels.");
}

static void part_cmd_format(const char *arg) {
    st_target_t t;
    if (!part_resolve_say(arg, &t, 0)) return;
    if (vfs_find_mount(t.id)) { term_print("  it is mounted: part unmount first"); return; }
    u32 vb, vn;
    int k = part_find_volume(&t, &vb, &vn);
    part_line("format ", t.id);
    { char line[TERM_COLS + 1]; u32 l = 0; append_str(line, &l, sizeof(line), "  "); term_fmt_size(line, &l, sizeof(line), t.sectors > PART_MAX_VOL ? PART_MAX_VOL : t.sectors); append_str(line, &l, sizeof(line), " MWFS volume"); term_print(line); }
    if (k > 0) term_print("  it already holds a MiniWin filesystem");
    if (t.whole) {
        st_part_t p[16];
        int n = st_list_partitions(t.pos, p, 16);
        if (n > 0) term_print("  destroys the partition table!");
    }
    term_print("  everything on it is erased");
    part_confirm(PART_OP_FORMAT, t.id, &t, 0);
}

/* ---------------------------------------------------------------- MBR editing */
#define MBR_ENTRY(m, i) ((m) + 446 + 16 * (i))
static void part_le32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

/* Loads LBA 0. Returns 1 for a valid MBR, 0 for a blank disk (m is then an empty, correctly signed MBR), -1 for GPT or an I/O error. */
static int part_mbr_load(int pos, u8 *m) {
    if (!st_read(pos, 0, m)) return -1;
    int sch = st_scheme(pos);
    if (sch == ST_SCHEME_GPT) return -1;
    if (sch == ST_SCHEME_MBR) return 1;
    for (int i = 0; i < 512; i++) m[i] = 0;
    m[510] = 0x55; m[511] = 0xAA;
    return 0;
}
static int part_mbr_store(int pos, const u8 *m) {
    if (!blk_write(pos, 0, m)) return 0;
    u8 back[512];
    if (!st_read(pos, 0, back)) return 0;
    for (int i = 0; i < 512; i++) if (back[i] != m[i]) return 0;
    return 1;
}
static void part_mbr_fill(u8 *e, u32 start, u32 sectors, u8 type, int active) {
    for (int i = 0; i < 16; i++) e[i] = 0;
    e[0] = active ? 0x80 : 0;
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;                  /* CHS: "use LBA" */
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    part_le32(e + 8, start); part_le32(e + 12, sectors);
}

/* Only primary entries of an MBR disk can be edited. */
static int part_editable(const st_target_t *t) {
    if (t->whole) { term_print("  pick a partition, not a whole disk"); return 0; }
    if (t->part < 1 || t->part > 4) { term_print_wrapped("only primary partitions 01-04 can be edited", 2); return 0; }
    if (st_scheme(t->pos) != ST_SCHEME_MBR) { term_print_wrapped("only MBR partition tables can be edited", 2); return 0; }
    return 1;
}

static void part_cmd_create(const char *arg, const char *mb) {
    if (!arg[0]) { term_print("  usage: part create <id> [megabytes]"); return; }
    st_id_t id;
    int r = st_parse_id(arg, &id);
    if (r != ST_OK) { term_print_wrapped(st_error_text(r), 2); return; }
    if (id.part < 1 || id.part > 4) { term_print("  new partitions are primary: 01-04"); return; }
    if (!st_type_has_driver(id.type) || id.type == ST_CD) { term_print_wrapped(st_error_text(id.type == ST_CD ? ST_E_READ_ONLY : ST_E_NO_DRIVER), 2); return; }
    if (!st_scanned) st_scan();
    int pos = blk_index(id.type, id.dev);
    if (pos < 0 || !st_devs[pos].present) { term_print_wrapped(st_error_text(ST_E_NO_DEVICE), 2); return; }
    u8 m[512];
    int have = part_mbr_load(pos, m);
    if (have < 0) { term_print_wrapped("that disk has a GPT table: not edited here", 2); return; }
    u8 *entry = MBR_ENTRY(m, (int)id.part - 1);
    if (have == 1 && st_le32(entry + 12) != 0 && entry[4] != 0) { term_print("  that partition number is taken"); return; }
    u32 total = st_devs[pos].sectors;
    u32 want = 0;
    if (mb[0]) {
        u32 v = 0;
        for (int i = 0; mb[i]; i++) { if (mb[i] < '0' || mb[i] > '9' || v > 2000000u) { term_print_wrapped("size is in megabytes: part create <id> 512", 2); return; } v = v * 10 + (u32)(mb[i] - '0'); }
        if (v == 0) { term_print("  size must be at least 1"); return; }
        want = v * 2048u;
    }
    /* free gaps: from 1 MB (the usual first-partition alignment) to the end, minus every used entry */
    u32 gs[5], gl[5]; int ng = 1;
    gs[0] = 2048; gl[0] = total > 2048 ? total - 2048 : 0;
    for (int i = 0; i < 4; i++) {
        u8 *e = MBR_ENTRY(m, i);
        u32 s = st_le32(e + 8), n = st_le32(e + 12);
        if (have != 1 || e[4] == 0 || n == 0) continue;
        for (int g = 0; g < ng; g++) {                          /* cut [s, s+n) out of every gap */
            u32 a = gs[g], b = gs[g] + gl[g];
            if (!(s < b && a < s + n)) continue;
            u32 left = s > a ? s - a : 0, right = (s + n) < b ? b - (s + n) : 0;
            if (left && right && ng < 5) { gs[ng] = s + n; gl[ng] = right; ng++; gl[g] = left; }
            else if (left) gl[g] = left;
            else if (right) { gs[g] = s + n; gl[g] = right; }
            else gl[g] = 0;
        }
    }
    int pick = -1;
    for (int g = 0; g < ng; g++) {
        u32 a = (gs[g] + 2047u) & ~2047u, end = gs[g] + gl[g];                  /* align the start up to 1 MB */
        if (a >= end) continue;
        u32 len = end - a;
        if (len < 2048) continue;
        if (want ? len >= want : 1) { if (pick < 0 || (!want && len > gl[pick])) { pick = g; gs[g] = a; gl[g] = len; } }
    }
    if (pick < 0) { term_print("  no free space big enough"); return; }
    u32 start = gs[pick], len = want ? want : gl[pick];
    if (start + len < start) len = 0xFFFFFFFFu - start;                           /* MBR's 32-bit limit */
    if (st_scheme(pos) == ST_SCHEME_NONE && have == 0) term_print("  (blank disk: writing a new MBR)");
    part_mbr_fill(entry, start, len, PART_NEW_TYPE, 0);
    if (!part_mbr_store(pos, m)) { term_print("  could not write the partition table"); return; }
    char nid[16]; st_format_id(id.type, id.dev, id.part, nid);
    part_line("  created ", nid);
    { char line[TERM_COLS + 1]; u32 l = 0; append_str(line, &l, sizeof(line), "  "); term_fmt_size(line, &l, sizeof(line), len); append_str(line, &l, sizeof(line), " at sector "); append_uint(line, &l, sizeof(line), (int)start); term_print(line); }
    term_print("  part format <id> to use it");
}

static void part_cmd_delete(const char *arg) {
    st_target_t t;
    if (!part_resolve_say(arg, &t, 0)) return;
    if (!part_editable(&t)) return;
    if (vfs_find_mount(t.id)) { term_print("  it is mounted: part unmount first"); return; }
    part_line("delete ", t.id);
    term_print("  removes it from the partition table");
    term_print("  (its contents become unreachable)");
    part_confirm(PART_OP_DELETE, t.id, &t, 0);
}

static void part_cmd_set(const char *arg, const char *what, const char *val) {
    st_target_t t;
    if (!part_resolve_say(arg, &t, 1)) return;
    if (!part_editable(&t)) return;
    u8 m[512];
    if (part_mbr_load(t.pos, m) != 1) { term_print("  no MBR to edit"); return; }
    u8 *e = MBR_ENTRY(m, (int)t.part - 1);
    if (term_streq(what, "active") || term_streq(what, "inactive")) {
        int on = term_streq(what, "active");
        if (on) for (int i = 0; i < 4; i++) MBR_ENTRY(m, i)[0] = 0;               /* only one bootable partition */
        e[0] = on ? 0x80 : 0;
    } else if (term_streq(what, "type") && val[0]) {
        u32 v = 0; int n = 0;
        for (; val[n]; n++) {
            char c = val[n]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0 || n >= 2) { term_print("  type is 1-2 hex digits"); return; }
            v = v * 16 + (u32)d;
        }
        if (v == 0 || st_is_extended_type((u8)v)) { term_print("  that type byte is not allowed here"); return; }
        e[4] = (u8)v;
    } else { term_print("  usage: part set <id> active|inactive"); term_print("         part set <id> type <hex>"); return; }
    if (!part_mbr_store(t.pos, m)) { term_print("  could not write the partition table"); return; }
    term_print("  done");
}

static void part_cmd_merge(const char *a, const char *b) {
    st_target_t t1, t2;
    if (!a[0] || !b[0]) { term_print("  usage: part merge <id1> <id2>"); return; }
    if (!part_resolve_say(a, &t1, 0) || !part_resolve_say(b, &t2, 0)) return;
    if (!part_editable(&t1) || !part_editable(&t2)) return;
    if (t1.pos != t2.pos || t1.part == t2.part) { term_print_wrapped("they must be two partitions of one disk", 2); return; }
    if (vfs_find_mount(t1.id) || vfs_find_mount(t2.id)) { term_print("  one is mounted: part unmount first"); return; }
    u32 lo_end = t1.base < t2.base ? t1.base + t1.sectors : t2.base + t2.sectors;
    u32 hi_start = t1.base < t2.base ? t2.base : t1.base;
    if (lo_end != hi_start) { term_print("  they are not next to each other"); return; }
    part_line("merge ", t1.id);
    part_line("  and ", t2.id);
    term_print("  into one partition; both are erased");
    part_confirm(PART_OP_MERGE, t1.id, &t1, &t2);
}

/* The YES that follows format / delete / merge. */
static void part_exec_pending(void) {
    part_pending.active = 0;
    st_target_t *t = &part_pending.t1;
    if (part_pending.op == PART_OP_FORMAT) {
        mwfs_vol_t v;
        mwfs_zero(&v, sizeof v);
        v.dev = t->pos; v.base = t->base; v.sectors = t->sectors > PART_MAX_VOL ? PART_MAX_VOL : t->sectors;
        term_print("  formatting...");
        int e = mwfs_mkfs(&v, "DATA");
        if (e != MWFS_OK) { term_fs_err("format", e); return; }
        term_print("  done: an empty MWFS volume");
        part_line("  part mount ", t->id);
    } else if (part_pending.op == PART_OP_DELETE) {
        u8 m[512];
        if (part_mbr_load(t->pos, m) != 1) { term_print("  no MBR to edit"); return; }
        u8 *e = MBR_ENTRY(m, (int)t->part - 1);
        if (st_le32(e + 8) != t->base) { term_print("  the table changed: nothing done"); return; }
        for (int i = 0; i < 16; i++) e[i] = 0;
        if (!part_mbr_store(t->pos, m)) { term_print("  could not write the partition table"); return; }
        term_print("  deleted");
    } else if (part_pending.op == PART_OP_MERGE) {
        st_target_t *u = &part_pending.t2;
        st_target_t *first = t->base < u->base ? t : u, *second = t->base < u->base ? u : t;
        u8 m[512];
        if (part_mbr_load(t->pos, m) != 1) { term_print("  no MBR to edit"); return; }
        u8 *e1 = MBR_ENTRY(m, (int)first->part - 1), *e2 = MBR_ENTRY(m, (int)second->part - 1);
        if (st_le32(e1 + 8) != first->base || st_le32(e2 + 8) != second->base) { term_print("  the table changed: nothing done"); return; }
        u32 sectors = first->sectors + second->sectors;
        if (sectors < first->sectors) { term_print("  too big for an MBR partition"); return; }
        part_le32(e1 + 12, sectors);
        for (int i = 0; i < 16; i++) e2[i] = 0;
        if (!part_mbr_store(t->pos, m)) { term_print("  could not write the partition table"); return; }
        part_line("  merged into ", first->id);
    }
}

/* ---------------------------------------------------------------- dispatcher */
static void part_help(void) {
    term_print("part - partitions and filesystems");
    term_print("  list            disks and partitions");
    term_print("  info <id>       details of one");
    term_print("  format <id>     new empty filesystem");
    term_print("  mount <id>      appears as /mnt/<id>");
    term_print("  unmount <id>");
    term_print("  check <id> [fix]  verify / repair");
    term_print("  create <id> [mb]  new MBR partition");
    term_print("  delete <id>     remove an MBR entry");
    term_print("  set <id> active|inactive|type <hex>");
    term_print("  merge <id1> <id2>  join two neighbours");
    term_print_wrapped("id like ide101, sata200 (00 = whole disk)", 0);
}
static void term_cmd_part(const char *c, const char *a, const char *b, const char *d) {
    if (!c[0] || term_streq(c, "?")) { part_help(); return; }
    if (term_streq(c, "list"))         part_cmd_list();
    else if (term_streq(c, "info"))    part_cmd_info(a);
    else if (term_streq(c, "format"))  part_cmd_format(a);
    else if (term_streq(c, "mount"))   part_cmd_mount(a);
    else if (term_streq(c, "unmount")) part_cmd_unmount(a);
    else if (term_streq(c, "check"))   part_cmd_check(a, b);
    else if (term_streq(c, "create"))  part_cmd_create(a, b);
    else if (term_streq(c, "delete"))  part_cmd_delete(a);
    else if (term_streq(c, "set"))     part_cmd_set(a, b, d);
    else if (term_streq(c, "merge"))   part_cmd_merge(a, b);
    else { part_line("  unknown: part ", c); term_print("  part ? lists the actions"); }
}

/* `usb`: what the USB stack found. */
static void term_cmd_usb(void) {
    if (!usb_hc_count) { term_print("  no USB host controller"); term_print("  (xhci, ehci and ohci are driven)"); return; }
    for (int h = 0; h < usb_hc_count; h++) {
        char line[TERM_COLS + 1]; u32 l = 0;
        append_str(line, &l, sizeof(line), "  ");
        append_str(line, &l, sizeof(line), usb_hc_names[usb_hcs[h].type]);
        append_uint(line, &l, sizeof(line), usb_hcs[h].idx);
        append_str(line, &l, sizeof(line), ": ");
        append_uint(line, &l, sizeof(line), usb_hcs[h].nports);
        append_str(line, &l, sizeof(line), " ports");
        term_print(line);
    }
    int n = 0;
    for (int i = 0; i < USB_MAX_DEVS; i++) {
        usb_dev_t *dv = &usb_devs[i];
        if (!dv->present || !dv->hid) continue;
        char line[TERM_COLS + 1]; u32 l = 0;
        static const char hx[] = "0123456789abcdef";
        append_str(line, &l, sizeof(line), "  ");
        append_str(line, &l, sizeof(line), dv->hid == HID_KBD ? "keyboard" : dv->hid == HID_MOUSE ? "mouse" : "tablet");
        append_str(line, &l, sizeof(line), " ");
        for (int k = 3; k >= 0; k--) { char c[2] = { hx[(dv->vid >> (4 * k)) & 15], 0 }; append_str(line, &l, sizeof(line), c); }
        append_str(line, &l, sizeof(line), ":");
        for (int k = 3; k >= 0; k--) { char c[2] = { hx[(dv->pid >> (4 * k)) & 15], 0 }; append_str(line, &l, sizeof(line), c); }
        append_str(line, &l, sizeof(line), " on ");
        append_str(line, &l, sizeof(line), usb_hc_names[usb_hcs[dv->hc].type]);
        append_uint(line, &l, sizeof(line), usb_hcs[dv->hc].idx);
        term_print(line);
        n++;
    }
    if (!n) term_print("  no keyboard/mouse attached");
}
#endif
