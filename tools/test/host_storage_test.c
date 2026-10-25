/* SPDX-License-Identifier: GPL-3.0-only */
/* host_storage_test.c -- kernel/storage.h against a fake IDE controller with FOUR positions (two channels x
 * master/slave), each holding a disk image, an ATAPI drive, or nothing. Covers device scanning, MBR / extended /
 * GPT partition parsing, storage-id parsing and resolution (including every refusal), and finding the boot
 * device by install id.   tools/test/run_host_storage.sh */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IO_H
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            i32;

/* ---------------------------------------------------------------- fake controller */
enum { DEV_NONE = 0, DEV_DISK, DEV_ATAPI };
#define MAXSEC 20000u
static int      dev_kind[4];
static u32      dev_sectors[4];
static int      dev_lba48[4];
static const char *dev_model[4];
static u8      *dev_img[4];
static int      chan_floating[2];

static int      sel_slave[2];
static u32      lba_reg[2];
static int      cmd[2], xfer[2];
static u16      ident[2][256];
static u8       wbuf[2][512];
static u8       lo_hi[2][2];
/* fault injection (per device 0..3) */
static long     writes_done[4];
static int      inj_write_fail_dev = -1;  static long inj_write_fail_after = -1;     /* writes beyond this many fail */
static int      inj_lost_dev = -1;        static u32  inj_lost_lba = 0xFFFFFFFFu;     /* this write is acknowledged but never stored */
static int      inj_read_fail_dev = -1;   static u32  inj_read_fail_lba = 0xFFFFFFFFu;
static int      cmd_err[2];

static int chan_of(u16 port) { return (port & 0xFFF8) == 0x1F0 ? 0 : (port & 0xFFF8) == 0x170 ? 1 : -1; }
static int cur_dev(int ch) { return ch * 2 + sel_slave[ch]; }

static void build_ident(int d) {
    int ch = d / 2;
    memset(ident[ch], 0, sizeof ident[ch]);
    char m[41]; memset(m, ' ', 40); m[40] = 0; memcpy(m, dev_model[d], strlen(dev_model[d]));
    for (int i = 0; i < 20; i++) ident[ch][27 + i] = (u16)(((u8)m[i * 2] << 8) | (u8)m[i * 2 + 1]);
    ident[ch][49] = 0x0200;
    ident[ch][60] = (u16)(dev_sectors[d] & 0xFFFF); ident[ch][61] = (u16)(dev_sectors[d] >> 16);
    if (dev_lba48[d]) { ident[ch][83] = 0x0400; ident[ch][100] = (u16)(dev_sectors[d] & 0xFFFF); ident[ch][101] = (u16)(dev_sectors[d] >> 16); }
}

static u8 inb(u16 port) {
    int ch = chan_of(port);
    if (ch < 0) return 0xFF;
    int reg = port & 7;
    int d = cur_dev(ch);
    if (reg == 7) {
        if (chan_floating[ch]) return 0xFF;
        if (dev_kind[d] == DEV_NONE) return 0x00;
        if (cmd_err[ch] && (cmd[ch] == 0x20 || cmd[ch] == 0x30)) return 0x41;
        if (cmd[ch] == 0x20 || cmd[ch] == 0x30 || cmd[ch] == 0xEC) return xfer[ch] < 256 ? 0x58 : 0x50;
        return 0x50;
    }
    if (reg == 4) return lo_hi[ch][0];
    if (reg == 5) return lo_hi[ch][1];
    return 0;
}
static void outb(u16 port, u8 v) {
    int ch = chan_of(port);
    if (ch < 0) return;
    int reg = port & 7;
    int d = cur_dev(ch);
    if (reg == 6) { sel_slave[ch] = (v >> 4) & 1; lba_reg[ch] = (lba_reg[ch] & 0x00FFFFFF) | ((u32)(v & 0x0F) << 24); }
    else if (reg == 3) lba_reg[ch] = (lba_reg[ch] & ~0xFFu) | v;
    else if (reg == 4) { lba_reg[ch] = (lba_reg[ch] & ~0xFF00u) | ((u32)v << 8); }
    else if (reg == 5) { lba_reg[ch] = (lba_reg[ch] & ~0xFF0000u) | ((u32)v << 16); }
    else if (reg == 7) {
        d = cur_dev(ch);
        cmd[ch] = v; xfer[ch] = 0; cmd_err[ch] = 0;
        if (v == 0x30 && inj_write_fail_dev == d && inj_write_fail_after >= 0 && writes_done[d] >= inj_write_fail_after) cmd_err[ch] = 1;
        if (v == 0x20 && inj_read_fail_dev == d && lba_reg[ch] == inj_read_fail_lba) cmd_err[ch] = 1;
        if (v == 0xEC) {
            lo_hi[ch][0] = lo_hi[ch][1] = 0;
            if (dev_kind[d] == DEV_ATAPI) { lo_hi[ch][0] = 0x14; lo_hi[ch][1] = 0xEB; cmd[ch] = 0; }
            else if (dev_kind[d] == DEV_DISK) build_ident(d);
        }
    }
}
static u16 inw(u16 port) {
    int ch = chan_of(port); int d = cur_dev(ch);
    u16 w;
    if (cmd[ch] == 0xEC) w = ident[ch][xfer[ch]];
    else { u32 lba = lba_reg[ch]; w = (lba < dev_sectors[d]) ? *(u16 *)&dev_img[d][lba * 512 + xfer[ch] * 2] : 0; }
    if (++xfer[ch] == 256) cmd[ch] = 0;
    return w;
}
static void outw(u16 port, u16 v) {
    int ch = chan_of(port); int d = cur_dev(ch);
    *(u16 *)&wbuf[ch][xfer[ch] * 2] = v;
    if (++xfer[ch] == 256) {
        if (lba_reg[ch] < dev_sectors[d] && !(inj_lost_dev == d && inj_lost_lba == lba_reg[ch])) memcpy(&dev_img[d][lba_reg[ch] * 512], wbuf[ch], 512);
        writes_done[d]++; cmd[ch] = 0;
    }
}
static void outl(u16 p, u32 v) { (void)p; (void)v; }
static u32  inl(u16 p) { (void)p; return 0; }
static void io_wait(void) {}

/* The sector the BIOS left at 0x7C00: st_boot_locate() fingerprints it. On the host it is this array. */
static u8 fake_bootsect[512];
#define MW_BOOTSECT_COPY ((const u8 *)fake_bootsect)
#define MW_BOOT_INFO8(addr) ((u8)0x80)          /* "booted from the first hard disk": the floppy driver stays out of it */

#include "../../kernel/storage.h"
#include "../../kernel/os_inst.h"

static int checks = 0, failures = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------- image builders */
static void reset_all(void) {
    for (int d = 0; d < 4; d++) { if (dev_img[d]) { free(dev_img[d]); dev_img[d] = 0; } dev_kind[d] = DEV_NONE; dev_sectors[d] = 0; dev_lba48[d] = 0; dev_model[d] = ""; }
    chan_floating[0] = chan_floating[1] = 0;
    st_scanned = 0; blk_boot_dev = 0; blk_boot_base = 0;
    memset(st_devs, 0, sizeof st_devs);
    memset(writes_done, 0, sizeof writes_done);
    inj_write_fail_dev = inj_lost_dev = inj_read_fail_dev = -1; inj_write_fail_after = -1; inj_lost_lba = inj_read_fail_lba = 0xFFFFFFFFu;
    inst_id_override = 0; memset(&inst, 0, sizeof inst);
}
static void add_disk(int d, u32 sectors, const char *model) {
    dev_kind[d] = DEV_DISK; dev_sectors[d] = sectors; dev_model[d] = model;
    dev_img[d] = calloc(sectors, 512);
}
static void put32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void mbr_entry(u8 *sector, int idx, u8 status, u8 type, u32 start, u32 count) {
    u8 *e = sector + 446 + idx * 16; e[0] = status; e[4] = type; put32(e + 8, start); put32(e + 12, count);
}
static void sign(u8 *s) { s[510] = 0x55; s[511] = 0xAA; }
static u8 *sec(int d, u32 lba) { return &dev_img[d][lba * 512]; }

/* ================================================================ installation */
static u8 expect_buf[512];
/* The source ("MiniWin running from dev 0 at `src_base`"): recognisable bytes in the system sectors, junk (0x5A) in the
 * user-data sectors, which must never be copied. */
static void fill_source(int dev, u32 src_base, u32 running_id) {
    for (u32 lba = 0; lba < ST_INSTALL_SECTORS; lba++) {
        u8 *sc = sec(dev, src_base + lba);
        if (inst_is_system_sector(lba)) for (int i = 0; i < 512; i++) sc[i] = (u8)((lba * 7 + i * 3 + 1) | 1);
        else memset(sc, 0x5A, 512);
    }
    u8 *b = sec(dev, src_base); b[510] = 0x55; b[511] = 0xAA;
    put32(b + ST_BOOT_FIELDS_OFF, src_base); put32(b + ST_BOOT_FIELDS_OFF + 4, running_id);
}
static void source_sector(int dev, u32 src_base, u32 lba, u8 *out) {
    if (inst_is_system_sector(lba)) memcpy(out, sec(dev, src_base + lba), 512); else memset(out, 0, 512);
}
/* Independent check: the target holds exactly the expected installation at `base` */
static int image_ok(int tdev, u32 base, u32 id, int sdev, u32 src_base, u32 *bad_lba) {
    for (u32 lba = 0; lba < MW_ROOTFS_LBA; lba++) {                  /* the copied part; the root filesystem after it is formatted, not copied */
        source_sector(sdev, src_base, lba, expect_buf);
        if (lba == 0) { put32(expect_buf + ST_BOOT_FIELDS_OFF, base); put32(expect_buf + ST_BOOT_FIELDS_OFF + 4, id); }
        if (memcmp(sec(tdev, base + lba), expect_buf, 512) != 0) { *bad_lba = lba; return 0; }
    }
    return 1;
}
static int run_job(void) { int guard = 0; while (inst_active() && guard++ < 200000) inst_step(37); return inst.phase; }
static void sentinel(int d) { memset(dev_img[d], 0xEE, (size_t)dev_sectors[d] * 512); }
static int all_sentinel(int d, u32 from, u32 to) { for (u32 l = from; l < to; l++) for (int i = 0; i < 512; i++) if (dev_img[d][l * 512 + i] != 0xEE) return 0; return 1; }

/* A fresh root volume must be openable in a target of `target_sectors` and hold the standard directories and nothing else. */
static int root_volume_ok(int dev, u32 base, u32 target_sectors, u32 *data_end) {
    mwfs_vol_t v; memset(&v, 0, sizeof v);
    v.dev = dev; v.base = base + MW_ROOTFS_LBA; v.sectors = target_sectors - MW_ROOTFS_LBA;
    if (mwfs_open(&v) != MWFS_OK || v.sectors != target_sectors - MW_ROOTFS_LBA) return 0;
    u32 ino; u8 t;
    if (mwfs_dir_lookup(&v, MWFS_ROOT_INO, "docs", &ino, &t) != MWFS_OK || mwfs_dir_lookup(&v, MWFS_ROOT_INO, "progs", &ino, &t) != MWFS_OK) return 0;
    mwfs_dirent_t e; u32 n = 0; while (mwfs_dir_nth(&v, MWFS_ROOT_INO, n, &e) == 1) n++;
    mwfs_report_t r;
    if (n != 4 || mwfs_check(&v, 0, &r) != MWFS_OK || r.bad_entries || r.orphans || r.leaked || r.unmarked) return 0;
    if (data_end) *data_end = MW_ROOTFS_LBA + v.data_lba;
    return 1;
}
static long full_writes = 0;                                  /* how many sector writes a complete install of the 8192-sector target takes */
static void test_install(void) {
    st_target_t t; u32 bad = 0, dend = 0;

    /* ---- 1. whole device ---- */
    reset_all(); add_disk(0, 4096, "RUNNING"); add_disk(2, 8192, "TARGET"); fill_source(0, 0, 0); sentinel(2);
    inst_id_override = 0x1234ABCD;
    CHECK(st_resolve("ide300", &t) == ST_OK, "resolve the whole target device");
    inst_begin(&t, 0);
    CHECK(inst.phase == INST_PHASE_WRITE && inst.id == 0x1234ABCD && inst_active(), "job starts");
    inst_step(10); CHECK(inst_percent() == 0 && inst.phase == INST_PHASE_WRITE, "progress starts at 0%% (%u)", inst_percent());
    CHECK(run_job() == INST_PHASE_DONE && inst.err == INST_ERR_NONE, "whole-device install completes (phase %d err %d lba %u)", inst.phase, inst.err, inst.err_lba);
    CHECK(image_ok(2, 0, 0x1234ABCD, 0, 0, &bad), "the target holds exactly the expected installation (first bad sector: %u)", bad);
    CHECK(root_volume_ok(2, 0, 8192, &dend), "the target got a fresh, clean root filesystem with /docs /progs /mnt /sys");
    CHECK(dend > MW_ROOTFS_LBA && all_sentinel(2, dend + 8, 8192), "the volume's data area (beyond its few directory sectors) was not touched (%u)", dend);
    full_writes = writes_done[2];
    CHECK(inst.mbr_result == INST_MBR_NONE && inst_percent() == 100, "no disk MBR logic for a whole-device install");
    CHECK(st_le32(sec(2, 0) + ST_BOOT_FIELDS_OFF) == 0 && st_le32(sec(2, 0) + ST_BOOT_FIELDS_OFF + 4) == 0x1234ABCD, "install_base 0 and the id are patched into the boot sector");
    CHECK(writes_done[2] >= MW_ROOTFS_LBA && writes_done[2] < MW_ROOTFS_LBA + 256, "the copied sectors plus a small filesystem format (got %ld writes)", writes_done[2]);
    /* the new installation can find itself at boot */
    st_boot_locate(0, 0x1234ABCD); CHECK(blk_boot_dev == 2 && blk_boot_base == 0, "booted from that disk, the kernel locates it by id (pos %d)", blk_boot_dev);
    blk_boot_dev = 0; blk_boot_base = 0;

    /* ---- 2. partition of a blank-MBR disk: chain loader added, partition marked active ---- */
    reset_all(); add_disk(0, 4096, "RUNNING"); add_disk(2, 16384, "TARGET"); fill_source(0, 0, 0); sentinel(2);
    { u8 *m = sec(2, 0); memset(m, 0, 512); mbr_entry(m, 0, 0x80, 0x83, 2048, 4096); mbr_entry(m, 1, 0x00, 0x83, 8192, 4096); sign(m); }
    inst_id_override = 0x0BADBEEF;
    CHECK(st_resolve("ide302", &t) == ST_OK && t.base == 8192, "resolve partition 2 (base %u)", t.base);
    { u8 before[512]; memcpy(before, sec(2, 0), 512);
      inst_begin(&t, 0); CHECK(run_job() == INST_PHASE_DONE, "partition install completes (err %d)", inst.err);
      CHECK(image_ok(2, 8192, 0x0BADBEEF, 0, 0, &bad), "the partition holds the installation, base %u (bad %u)", 8192u, bad);
      CHECK(st_le32(sec(2, 8192) + ST_BOOT_FIELDS_OFF) == 8192, "its boot sector says it starts at LBA 8192");
      CHECK(inst.mbr_result == INST_MBR_CHAIN, "blank MBR code area: chain loader installed (result %d)", inst.mbr_result);
      u8 *m = sec(2, 0);
      CHECK(memcmp(m, chain_mbr_code, CHAIN_MBR_SIZE) == 0, "the chain loader is in bytes 0-445");
      CHECK(m[446] == 0x00 && m[446 + 16] == 0x80, "partition 2 is now the active one, partition 1 no longer is");
      CHECK(memcmp(m + 446 + 1, before + 446 + 1, 15) == 0 && memcmp(m + 446 + 16 + 1, before + 446 + 16 + 1, 15) == 0, "the rest of the partition table is unchanged");
      CHECK(m[510] == 0x55 && m[511] == 0xAA, "the 0xAA55 signature is still there");
      CHECK(all_sentinel(2, 1, 2048) && all_sentinel(2, 8192 + ST_INSTALL_SECTORS, 16384), "nothing outside the target partition (and the MBR) was touched"); }

    /* ---- 3. partition of a disk that already has boot code: hands off ---- */
    reset_all(); add_disk(0, 4096, "RUNNING"); add_disk(2, 16384, "TARGET"); fill_source(0, 0, 0); sentinel(2);
    { u8 *m = sec(2, 0); memset(m, 0, 512); m[0] = 0xEB; m[1] = 0x5A; memcpy(m + 3, "GRUB LOADER", 11); mbr_entry(m, 0, 0x80, 0x83, 2048, 4096); mbr_entry(m, 1, 0, 0x83, 8192, 4096); sign(m); }
    { u8 before[512]; memcpy(before, sec(2, 0), 512);
      st_resolve("ide302", &t); inst_begin(&t, 0);
      CHECK(run_job() == INST_PHASE_DONE && inst.mbr_result == INST_MBR_KEPT, "existing boot code: install done, MBR left alone (result %d)", inst.mbr_result);
      CHECK(memcmp(before, sec(2, 0), 512) == 0, "the disk's sector 0 is byte-for-byte unchanged"); }

    /* ---- 4. logical partition on a blank MBR: installable, but the chain loader cannot start it ---- */
    reset_all(); add_disk(0, 4096, "RUNNING"); add_disk(2, 16384, "TARGET"); fill_source(0, 0, 0); sentinel(2);
    { u8 *m = sec(2, 0); memset(m, 0, 512); mbr_entry(m, 0, 0x80, 0x83, 2048, 2048); mbr_entry(m, 1, 0, 0x05, 4096, 10000); sign(m);
      u8 *e = sec(2, 4096); memset(e, 0, 512); mbr_entry(e, 0, 0, 0x83, 64, 4000); sign(e); }
    { u8 before[512]; memcpy(before, sec(2, 0), 512);
      CHECK(st_resolve("ide303", &t) == ST_E_NO_PARTITION, "(no partition 3 here)");
      CHECK(st_resolve("ide305", &t) == ST_OK, "logical partition 5 resolves (base %u)", t.base);
      inst_begin(&t, 0); CHECK(run_job() == INST_PHASE_DONE && inst.mbr_result == INST_MBR_UNSUPPORTED, "installed, but flagged: cannot be chain-loaded (result %d)", inst.mbr_result);
      CHECK(memcmp(before, sec(2, 0), 512) == 0, "the MBR is untouched"); CHECK(image_ok(2, 4096 + 64, inst.id, 0, 0, &bad), "the installation itself is complete"); }

    /* ---- 5. GPT disk ---- */
    reset_all(); add_disk(0, 4096, "RUNNING"); add_disk(1, 16384, "GPT"); fill_source(0, 0, 0); sentinel(1);
    { u8 *m = sec(1, 0); memset(m, 0, 512); mbr_entry(m, 0, 0, 0xEE, 1, 16383); sign(m);
      u8 *h = sec(1, 1); memset(h, 0, 512); memcpy(h, "EFI PART", 8); put32(h + 72, 2); put32(h + 80, 128); put32(h + 84, 128);
      u8 *ent = sec(1, 2); memset(ent, 0, 512); memset(ent, 0xAA, 16); put32(ent + 32, 4096); put32(ent + 40, 8191); }
    { u8 before[512]; memcpy(before, sec(1, 0), 512);
      CHECK(st_resolve("ide201", &t) == ST_OK && t.base == 4096, "gpt partition 1 resolves");
      inst_begin(&t, 0); CHECK(run_job() == INST_PHASE_DONE && inst.mbr_result == INST_MBR_UNSUPPORTED, "gpt: installed, the protective MBR is not touched (result %d)", inst.mbr_result);
      CHECK(memcmp(before, sec(1, 0), 512) == 0, "protective MBR unchanged"); }

    /* ---- 6. the target refuses a write part-way: never bootable, old sector 0 survives ---- */
    /* a write refused during the COPY is a write error at that sector; one refused during the filesystem FORMAT is a format error;
     * one refused at the very last write (the boot sector itself) names sector 0. In every case sector 0 still holds the old table. */
    long cuts[] = { 0, 1, 100, 1500, 1731, 1733, 1760, full_writes - 2, full_writes - 1 };
    for (unsigned c = 0; c < sizeof cuts / sizeof cuts[0]; c++) {
        reset_all(); add_disk(0, 4096, "RUNNING"); add_disk(2, 8192, "TARGET"); fill_source(0, 0, 0); sentinel(2);
        u8 *m = sec(2, 0); memset(m, 0, 512); mbr_entry(m, 0, 0x80, 0x83, 2048, 4096); sign(m);   /* the disk's existing table */
        u8 before[512]; memcpy(before, m, 512);
        st_resolve("ide300", &t); inj_write_fail_dev = 2; inj_write_fail_after = cuts[c];
        inst_begin(&t, 0);
        int in_copy = cuts[c] < (long)MW_ROOTFS_LBA, is_last = cuts[c] == full_writes - 1;
        int want_err = (in_copy || is_last) ? INST_ERR_WRITE : INST_ERR_FORMAT;
        u32 want_lba = in_copy ? inst_lba_for((u32)cuts[c]) : is_last ? 0 : MW_ROOTFS_LBA;
        CHECK(run_job() == INST_PHASE_FAILED && inst.err == want_err, "write refused after %ld sectors: job fails (phase %d err %d, wanted %d)", cuts[c], inst.phase, inst.err, want_err);
        CHECK(memcmp(before, sec(2, 0), 512) == 0 || is_last, "...and the old sector 0 / partition table is intact (it is written last), after %ld writes", cuts[c]);
        CHECK(inst.err_lba == want_lba, "...the error names the right sector (%u, wanted %u)", inst.err_lba, want_lba);
    }

    /* ---- 7. a write that is acknowledged but silently lost is caught by the read-back ---- */
    reset_all(); add_disk(0, 4096, "RUNNING"); add_disk(2, 8192, "TARGET"); fill_source(0, 0, 0); sentinel(2);
    st_resolve("ide300", &t); inj_lost_dev = 2; inj_lost_lba = 777;
    inst_begin(&t, 0);
    CHECK(run_job() == INST_PHASE_FAILED && inst.err == INST_ERR_VERIFY && inst.err_lba == 777, "a lost write is detected at verify: err %d sector %u", inst.err, inst.err_lba);

    /* ---- 8. MiniWin's own disk cannot be read ---- */
    reset_all(); add_disk(0, 4096, "RUNNING"); add_disk(2, 8192, "TARGET"); fill_source(0, 0, 0); sentinel(2);
    { u8 *m = sec(2, 0); memset(m, 0, 512); mbr_entry(m, 0, 0x80, 0x83, 2048, 4096); sign(m); }
    { u8 before[512]; memcpy(before, sec(2, 0), 512);
      st_resolve("ide300", &t); inj_read_fail_dev = 0; inj_read_fail_lba = 500;
      inst_begin(&t, 0); CHECK(run_job() == INST_PHASE_FAILED && inst.err == INST_ERR_READ_SOURCE && inst.err_lba == 500, "source read error: err %d sector %u", inst.err, inst.err_lba);
      CHECK(memcmp(before, sec(2, 0), 512) == 0, "the target's sector 0 is untouched"); }

    /* ---- 9. running from a partition itself: sources are read relative to MiniWin's own base ---- */
    reset_all(); add_disk(0, 16384, "RUNNING"); add_disk(2, 8192, "TARGET"); fill_source(0, 4096, 0xAAAA0001); sentinel(2);
    blk_boot_dev = 0; blk_boot_base = 4096;
    st_resolve("ide300", &t); inj_read_fail_dev = -1;
    inst_id_override = 0; inst_begin(&t, 0xAAAA0001);
    CHECK(inst.id != 0 && inst.id != 0xAAAA0001, "a fresh id, different from the running system's (0x%08X)", inst.id);
    CHECK(run_job() == INST_PHASE_DONE && image_ok(2, 0, inst.id, 0, 4096, &bad), "an installed system can install a copy of itself (bad %u)", bad);
    CHECK(st_le32(sec(2, 0) + ST_BOOT_FIELDS_OFF + 4) != 0xAAAA0001, "the copy does not inherit the old install id");
    blk_boot_dev = 0; blk_boot_base = 0;
    { u32 ids[64]; int distinct = 1; reset_all();
      for (int i = 0; i < 64; i++) { ids[i] = inst_new_id(i ? ids[i - 1] : 0); for (int k = 0; k < i; k++) if (ids[k] == ids[i]) distinct = 0; if (!ids[i]) distinct = 0; }
      CHECK(distinct, "64 generated install ids are non-zero and distinct"); }

    /* ---- 10. probing the target for the confirmation prompt ---- */
    reset_all(); add_disk(0, 4096, "RUNNING"); add_disk(2, 16384, "TARGET"); fill_source(0, 0, 0);
    { u8 *m = sec(2, 0); mbr_entry(m, 0, 0x80, 0x83, 2048, 4096); mbr_entry(m, 1, 0, 0x83, 8192, 4096); sign(m);
      memset(sec(2, 8192), 0, 512); }
    { inst_probe_t pr; st_resolve("ide300", &t); inst_probe(&t, &pr);
      CHECK(pr.readable && pr.non_empty && pr.has_mbr_sig && pr.partitions == 2 && !pr.miniwin, "whole-device probe: has a partition table with %d partitions to destroy", pr.partitions);
      st_resolve("ide302", &t); inst_probe(&t, &pr);
      CHECK(pr.readable && !pr.non_empty && !pr.miniwin, "an empty partition probes as empty");
      memcpy(sec(2, 8192), sec(0, 0), 512); put32(sec(2, 8192) + ST_BOOT_FIELDS_OFF + 4, 0x77); inst_probe(&t, &pr);
      CHECK(pr.miniwin, "a partition that already holds a MiniWin (install id present) is recognised"); }
}

int main(void) {
    /* ---------------- scanning ---------------- */
    reset_all();
    add_disk(0, 8192, "BOOT DISK");
    dev_kind[1] = DEV_ATAPI; dev_model[1] = "CDROM";                      /* slave on the primary channel: an ATAPI drive */
    add_disk(3, 20000, "SECOND CHANNEL SLAVE");                           /* secondary slave; secondary master is empty */
    st_scan();
    CHECK(st_devs[0].present && st_devs[0].sectors == 8192 && strcmp(st_devs[0].model, "BOOT DISK") == 0, "ide1 found: %u sectors, model [%s]", st_devs[0].sectors, st_devs[0].model);
    CHECK(!st_devs[1].present, "an ATAPI drive is not offered as a disk");
    CHECK(!st_devs[2].present, "an empty position is not a device");
    CHECK(st_devs[3].present && st_devs[3].sectors == 20000, "ide4 (secondary slave) found independently of ide3 being empty");
    reset_all(); chan_floating[0] = chan_floating[1] = 1; st_scan();
    CHECK(!st_devs[0].present && !st_devs[1].present && !st_devs[2].present && !st_devs[3].present, "floating buses (0xFF) yield no devices");
    reset_all(); add_disk(0, 8192, "BIG"); dev_lba48[0] = 1; dev_sectors[0] = 8192; st_scan();
    CHECK(st_devs[0].present && st_devs[0].sectors == 8192, "a 48-bit-capable disk reports its size");

    /* ---------------- id parsing ---------------- */
    struct { const char *s; int rc; int type; u32 dev, part; } ids[] = {
        { "ide101",   ST_OK, ST_IDE,  1, 1 },   { "sata103", ST_OK, ST_SATA, 1, 3 },  { "nvme200", ST_OK, ST_NVME, 2, 0 },
        { "scsi312",  ST_OK, ST_SCSI, 3, 12 },  { "usb101",  ST_OK, ST_USB,  1, 1 },  { "IDE101",  ST_OK, ST_IDE,  1, 1 },
        { "Sata103",  ST_OK, ST_SATA, 1, 3 },   { "ide1000", ST_OK, ST_IDE, 10, 0 },  { "sata1205", ST_OK, ST_SATA, 12, 5 },
        { "ide499",   ST_OK, ST_IDE,  4, 99 },
        { "",         ST_E_SYNTAX },  { "ide",      ST_E_SYNTAX }, { "ide1",     ST_E_SYNTAX }, { "ide10", ST_E_SYNTAX },
        { "101",      ST_E_SYNTAX },  { "ide001",   ST_E_SYNTAX }, { "ide0",     ST_E_SYNTAX },  { "ide1 01", ST_E_SYNTAX },
        { "ide1-01",  ST_E_SYNTAX },  { "ide101x",  ST_E_SYNTAX }, { "sata/103", ST_E_SYNTAX }, { "ide123456789", ST_E_SYNTAX },
        { "hda101",   ST_E_UNKNOWN_TYPE }, { "sda101", ST_E_UNKNOWN_TYPE }, { "floppy101", ST_E_UNKNOWN_TYPE }, { "ides101", ST_E_UNKNOWN_TYPE },
    };
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        st_id_t id; memset(&id, 0, sizeof id);
        int rc = st_parse_id(ids[i].s, &id);
        CHECK(rc == ids[i].rc, "parse [%s] -> rc %d (want %d)", ids[i].s, rc, ids[i].rc);
        if (rc == ST_OK && ids[i].rc == ST_OK) CHECK((int)id.type == ids[i].type && id.dev == ids[i].dev && id.part == ids[i].part, "parse [%s] -> type %d dev %u part %u", ids[i].s, id.type, id.dev, id.part);
    }
    { char b[16]; st_format_id(ST_SATA, 1, 3, b); CHECK(strcmp(b, "sata103") == 0, "format sata/1/3 -> [%s]", b);
      st_format_id(ST_IDE, 12, 0, b); CHECK(strcmp(b, "ide1200") == 0, "format ide/12/0 -> [%s]", b);
      st_format_id(ST_NVME, 2, 45, b); CHECK(strcmp(b, "nvme245") == 0, "format nvme/2/45 -> [%s]", b);
      st_id_t id; st_parse_id("scsi2307", &id); st_format_id(id.type, id.dev, id.part, b); CHECK(strcmp(b, "scsi2307") == 0, "parse/format roundtrip [%s]", b); }

    /* ---------------- MBR partitions ---------------- */
    reset_all(); add_disk(0, 8192, "BOOT"); add_disk(2, 16384, "TARGET");
    { u8 *m = sec(2, 0);
      mbr_entry(m, 0, 0x80, 0x83, 2048, 4096);
      mbr_entry(m, 1, 0x00, 0x0C, 6144, 2048);
      mbr_entry(m, 2, 0x00, 0x05, 8192, 8000);                            /* extended container */
      sign(m);
      /* EBR chain inside the extended partition (starts at 8192): 3 logical partitions */
      u8 *e1 = sec(2, 8192); mbr_entry(e1, 0, 0, 0x83, 64, 2048);  mbr_entry(e1, 1, 0, 0x05, 2200, 100); sign(e1);
      u8 *e2 = sec(2, 8192 + 2200); mbr_entry(e2, 0, 0, 0x83, 64, 2500); mbr_entry(e2, 1, 0, 0x05, 4800, 100); sign(e2);
      u8 *e3 = sec(2, 8192 + 4800); mbr_entry(e3, 0, 0, 0x07, 64, 3000); sign(e3); }
    st_part_t p;
    CHECK(st_scheme(2) == ST_SCHEME_MBR, "scheme: MBR");
    CHECK(st_get_partition(2, 1, &p) && p.start == 2048 && p.sectors == 4096 && p.active && p.type == 0x83 && !p.extended_container, "partition 1: start %u len %u active %d", p.start, p.sectors, p.active);
    CHECK(st_get_partition(2, 2, &p) && p.start == 6144 && p.sectors == 2048 && !p.active, "partition 2");
    CHECK(st_get_partition(2, 3, &p) && p.extended_container, "partition 3 is flagged as an extended container");
    CHECK(!st_get_partition(2, 4, &p), "partition 4 is empty");
    CHECK(st_get_partition(2, 5, &p) && p.start == 8192 + 64 && p.sectors == 2048, "logical 5: start %u (relative to its own EBR)", p.start);
    CHECK(st_get_partition(2, 6, &p) && p.start == 8192 + 2200 + 64 && p.sectors == 2500, "logical 6: start %u", p.start);
    CHECK(st_get_partition(2, 7, &p) && p.start == 8192 + 4800 + 64 && p.sectors == 3000, "logical 7: start %u", p.start);
    CHECK(!st_get_partition(2, 8, &p), "no logical 8");
    CHECK(!st_get_partition(2, 0, &p) && !st_get_partition(2, 100, &p), "numbers 0 and 100 are never partitions");
    { st_part_t list[16]; int n = st_list_partitions(2, list, 16);
      CHECK(n == 6 && list[0].number == 1 && list[1].number == 2 && list[2].number == 3 && list[3].number == 5 && list[4].number == 6 && list[5].number == 7,
            "listing returns the partitions in number order, container included (%d entries)", n); }
    /* a cyclic EBR chain must terminate */
    { u8 *e3 = sec(2, 8192 + 4800); mbr_entry(e3, 1, 0, 0x05, 0, 100);    /* "next" points back to the first EBR */
      CHECK(!st_get_partition(2, 12, &p), "a cyclic EBR chain terminates instead of looping"); }
    /* no table */
    CHECK(st_scheme(0) == ST_SCHEME_NONE && !st_get_partition(0, 1, &p), "a blank disk has no partitions");
    { memset(sec(0, 0), 0, 512); sign(sec(0, 0)); CHECK(st_scheme(0) == ST_SCHEME_NONE, "a signature with an empty table is not a partition table"); }

    /* ---------------- GPT ---------------- */
    reset_all(); add_disk(0, 8192, "BOOT"); add_disk(1, 16384, "GPT DISK");
    { u8 *m = sec(1, 0); mbr_entry(m, 0, 0, 0xEE, 1, 16383); sign(m);
      u8 *h = sec(1, 1); memcpy(h, "EFI PART", 8); put32(h + 72, 2); put32(h + 80, 128); put32(h + 84, 128);
      u8 *ent = sec(1, 2);                                                /* 4 entries per sector */
      memset(ent + 0 * 128, 0xAA, 16); put32(ent + 0 * 128 + 32, 2048); put32(ent + 0 * 128 + 40, 6143);      /* entry 1 */
      memset(ent + 2 * 128, 0xBB, 16); put32(ent + 2 * 128 + 32, 6144); put32(ent + 2 * 128 + 40, 12287);     /* entry 3 (entry 2 empty) */
      memset(ent + 3 * 128, 0xCC, 16); put32(ent + 3 * 128 + 32, 12288); put32(ent + 3 * 128 + 36, 1); put32(ent + 3 * 128 + 40, 12999); /* entry 4: 64-bit LBA */
      u8 *ent2 = sec(1, 3); memset(ent2, 0xDD, 16); put32(ent2 + 32, 13000); put32(ent2 + 40, 12999 - 1); }    /* entry 5: last < first */
    CHECK(st_scheme(1) == ST_SCHEME_GPT, "scheme: GPT");
    CHECK(st_get_partition(1, 1, &p) && p.is_gpt && p.start == 2048 && p.sectors == 4096, "gpt partition 1: %u+%u", p.start, p.sectors);
    CHECK(!st_get_partition(1, 2, &p), "gpt entry 2 is empty -> no partition 2");
    CHECK(st_get_partition(1, 3, &p) && p.start == 6144 && p.sectors == 6144, "gpt partition 3 keeps its entry number: %u+%u", p.start, p.sectors);
    CHECK(!st_get_partition(1, 4, &p), "a 64-bit LBA entry is refused, not truncated");
    CHECK(!st_get_partition(1, 5, &p), "an entry whose last LBA < first is refused");
    CHECK(!st_get_partition(1, 129, &p), "entry numbers past the table size do not exist");
    { st_part_t list[8]; int n = st_list_partitions(1, list, 8); CHECK(n == 2 && list[0].number == 1 && list[1].number == 3, "gpt listing: entries 1 and 3 (%d)", n); }
    { memcpy(sec(1, 1), "EFI PARX", 8); CHECK(st_scheme(1) == ST_SCHEME_MBR, "a protective MBR without a valid GPT header is treated as the MBR it is"); }

    /* ---------------- resolve ---------------- */
    reset_all(); add_disk(0, 8192, "BOOT"); add_disk(1, 3000, "SMALL"); add_disk(2, 16384, "TARGET");
    { u8 *m = sec(2, 0); mbr_entry(m, 0, 0x80, 0x83, 2048, 4096); mbr_entry(m, 1, 0, 0x83, 6144, 1000); mbr_entry(m, 2, 0, 0x05, 8192, 8000);
      mbr_entry(m, 3, 0, 0x83, 16000, 4000); sign(m);                      /* entry 4 runs past the end of the device */
      u8 *e1 = sec(2, 8192); mbr_entry(e1, 0, 0, 0x83, 64, 3000); sign(e1); }
    { u8 *m = sec(0, 0); mbr_entry(m, 0, 0x80, 0x83, 2048, 4096); sign(m); }   /* the boot disk has a partition table of its own */
    st_target_t t;
    CHECK(st_resolve("ide301", &t) == ST_OK && t.pos == 2 && t.base == 2048 && t.sectors == 4096 && !t.whole && strcmp(t.id, "ide301") == 0, "resolve ide301: pos %d base %u sectors %u", t.pos, t.base, t.sectors);
    CHECK(st_resolve("IDE300", &t) == ST_OK && t.whole && t.base == 0 && t.sectors == 16384, "resolve IDE300 (whole device, upper case)");
    CHECK(st_resolve("ide305", &t) == ST_OK && t.base == 8192 + 64 && t.sectors == 3000, "resolve a logical partition: base %u", t.base);
    CHECK(st_resolve("ide302", &t) == ST_E_TOO_SMALL, "a 1000-sector partition is too small for a 2048-sector installation");
    CHECK(st_resolve("ide303", &t) == ST_E_EXTENDED, "the extended container is not installable");
    CHECK(st_resolve("ide304", &t) == ST_E_RANGE, "a partition that runs past the end of the device is refused");
    CHECK(st_resolve("ide306", &t) == ST_E_NO_PARTITION, "no such partition");
    CHECK(st_resolve("ide200", &t) == ST_OK, "a 3000-sector whole disk is big enough");
    CHECK(st_resolve("ide400", &t) == ST_E_NO_DEVICE, "an empty position: no such device");
    CHECK(st_resolve("ide501", &t) == ST_E_NO_DEVICE, "device 5 does not exist on IDE");
    CHECK(st_resolve("usb101", &t) == ST_E_NO_DRIVER, "a type without a driver (usb) is named as such");
    CHECK(st_resolve("sata101", &t) == ST_E_NO_DEVICE && st_resolve("sata900", &t) == ST_E_NO_DEVICE
          && st_resolve("scsi101", &t) == ST_E_NO_DEVICE && st_resolve("scsi900", &t) == ST_E_NO_DEVICE
          && st_resolve("nvme101", &t) == ST_E_NO_DEVICE && st_resolve("nvme900", &t) == ST_E_NO_DEVICE,
          "sata, scsi and nvme have drivers: with no such disk attached the answer is 'no such device'");
    CHECK(st_resolve("hdx101", &t) == ST_E_UNKNOWN_TYPE && st_resolve("ide1", &t) == ST_E_SYNTAX, "syntax errors come through");

    /* never the running system */
    blk_boot_dev = 0; blk_boot_base = 0;
    CHECK(st_resolve("ide100", &t) == ST_E_OVERLAP_BOOT, "the whole boot disk overlaps the running installation");
    CHECK(st_resolve("ide101", &t) == ST_OK, "...but a partition that starts after the running installation is fine");
    blk_boot_base = 2048;                                                     /* running from inside partition 1 of ide1 */
    CHECK(st_resolve("ide101", &t) == ST_E_OVERLAP_BOOT, "the partition MiniWin runs from is refused");
    CHECK(st_resolve("ide100", &t) == ST_E_OVERLAP_BOOT, "so is the whole disk that contains it");
    CHECK(st_resolve("ide300", &t) == ST_OK, "another disk is fine");
    blk_boot_dev = 2; blk_boot_base = 0;
    CHECK(st_resolve("ide300", &t) == ST_E_OVERLAP_BOOT, "running from ide3 (base 0): its whole disk is refused");
    CHECK(st_resolve("ide301", &t) == ST_OK, "...but its partition 1 starts after the running installation, so it is allowed");
    CHECK(st_resolve("ide302", &t) == ST_E_TOO_SMALL, "(ide302 is still too small -- size is checked before overlap)");
    CHECK(st_resolve("ide305", &t) == ST_OK, "...while a logical partition beyond the running installation is allowed");
    blk_boot_dev = 0; blk_boot_base = 0;

    /* ---------------- finding the boot device ---------------- */
    reset_all(); add_disk(0, 8192, "A"); add_disk(2, 8192, "B"); add_disk(3, 8192, "C");
    put32(sec(2, 100) + ST_BOOT_FIELDS_OFF, 100); put32(sec(2, 100) + ST_BOOT_FIELDS_OFF + 4, 0xCAFEF00D);   /* installed at base 100 on ide3 */
    st_boot_locate(100, 0xCAFEF00D);
    CHECK(blk_boot_dev == 2 && blk_boot_base == 100, "boot device located by install id: pos %d base %u", blk_boot_dev, blk_boot_base);
    st_boot_locate(100, 0x12345678);
    CHECK(blk_boot_dev == 0 && blk_boot_base == 100, "unknown id: falls back to the primary master, base still applied");
    /* a stock image (id 0): the disk whose sector 0 is what the BIOS put at 0x7C00 -- code prefix + everything from the install fields on */
    memset(fake_bootsect, 0, 512); for (int i = 0; i < ST_BOOT_CODE_BYTES; i++) fake_bootsect[i] = (u8)(0xB0 + i); fake_bootsect[510] = 0x55; fake_bootsect[511] = 0xAA;
    reset_all(); add_disk(0, 8192, "A"); add_disk(2, 8192, "B"); add_disk(3, 8192, "C");
    memcpy(sec(2, 0), fake_bootsect, 512);                                                     /* the boot sector sits on ide3 */
    sec(2, 0)[100] ^= 0x5A; fake_bootsect[100] = sec(2, 0)[100] ^ 0x5A;                         /* (boot.asm rewrites its own disk-address packet in memory: bytes in between may differ) */
    st_boot_locate(0, 0);
    CHECK(blk_boot_dev == 2 && blk_boot_base == 0, "a stock image is found by comparing sector 0 with the BIOS's copy: pos %d", blk_boot_dev);
    memcpy(sec(3, 0), fake_bootsect, 512); sec(3, 0)[100] ^= 1;
    st_boot_locate(0, 0);
    CHECK(blk_boot_dev == 2, "an identical boot sector on a later disk does not steal it (first match wins)");
    sec(2, 0)[ST_BOOT_FIELDS_OFF + 20] ^= 0xFF;                                                 /* damage the tail: ide3 no longer matches, ide4 does */
    st_boot_locate(0, 0);
    CHECK(blk_boot_dev == 3, "the install-field/partition-table tail is part of the fingerprint (pos %d)", blk_boot_dev);
    reset_all(); add_disk(0, 8192, "A");
    st_boot_locate(0, 0);
    CHECK(blk_boot_dev == 0 && blk_boot_base == 0, "nothing matches (booted from a bus this build cannot drive): falls back to ide1");
    reset_all(); add_disk(0, 8192, "A"); add_disk(2, 8192, "B"); add_disk(3, 8192, "C");
    put32(sec(2, 100) + ST_BOOT_FIELDS_OFF, 100); put32(sec(2, 100) + ST_BOOT_FIELDS_OFF + 4, 0xCAFEF00D);
    put32(sec(3, 100) + ST_BOOT_FIELDS_OFF, 100); put32(sec(3, 100) + ST_BOOT_FIELDS_OFF + 4, 0xCAFEF00D);   /* a clone on ide4 */
    st_boot_locate(100, 0xCAFEF00D);
    CHECK(blk_boot_dev == 2, "two disks with the same id: the first position wins, deterministically");
    blk_boot_dev = 0; blk_boot_base = 0;

    test_install();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
