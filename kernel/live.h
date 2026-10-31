/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_LIVE_H
#define MW_LIVE_H

/* live.h -- the LIVE DISC (1.1-alpha-13): MiniWin run from the .iso, without touching any disk.
 *
 * THE PROBLEM. A CD boots through El Torito: the BIOS finds a "boot image" on the disc and presents it to the boot loader as an
 * ordinary hard disk (that is how the loader, which reads by INT 13h, gets stage 2 and the kernel into memory unchanged). But that
 * disk exists only inside the BIOS. The moment the kernel switches to protected mode, only the REAL devices on the buses it has drivers
 * for are visible -- the disc in the CD drive (atapi.h), not the disk image the BIOS showed -- so st_boot_locate() finds no device
 * that carries our boot sector, and the root volume (icons, programs, documents) has nowhere to be read from.
 *
 * THE FIX. The disc is there, as cd1..cd4, and an El Torito disc says WHERE its boot image is. So:
 *   1. tools/mkiso.py stamps the image's first sector with "LIVE" and the image's size (boot/boot.asm's live fields) in the disc's
 *      copy only, and builds the ISO (El Torito, hard-disk emulation, 5 MB at the very most).
 *   2. Here: if the sector the BIOS put at 0x7C00 carries that stamp, read each optical drive's El Torito boot catalog (volume
 *      descriptor 17 -> catalog -> the default entry's load address), check that the image's first sector IS our boot sector, then COPY
 *      THE WHOLE IMAGE into RAM and make that RAM the boot device (blk.h BLK_RAM_DEV, base 0). From there on everything -- icons,
 *      programs, /docs, the root volume -- works exactly as from a disk, and WRITES work too: they go to RAM and are gone at the next
 *      reboot (a live disc is a live disc). The installer (apps/installer.h) copies from that RAM image onto a real disk.
 * A disc is read 2048 bytes at a time; atapi.h serves it as four 512-byte sectors, so the arithmetic below is blocks * 4. */

#define LIVE_MAGIC        0x4556494Cu        /* 'L','I','V','E' read as a little-endian u32 */
#define LIVE_FIELD_OFF    (ST_BOOT_FIELDS_OFF - 8)
#define LIVE_MAX_SECTORS  10240u             /* 5 MB: the size the ISO may never exceed */
static int live_boot = 0;                    /* 1: this boot runs from a RAM copy of the disc's image (the installer shows up only then) */

static inline int live_read_block(int d, u32 block, u8 *out) {       /* one 2048-byte disc block as four 512-byte sectors */
    for (int i = 0; i < 4; i++) if (!blk_read(d, block * 4u + (u32)i, out + i * 512)) return 0;
    return 1;
}

/* The El Torito boot image of the disc in optical drive `d`: its first 2048-byte block, or 0 when there is none. */
static u32 live_find_image(int d) {
    static u8 blk[2048];
    if (!live_read_block(d, 17, blk)) return 0;                      /* the Boot Record Volume Descriptor is always block 17 */
    static const char sig[] = "EL TORITO SPECIFICATION";
    if (blk[0] != 0 || blk[1] != 'C' || blk[2] != 'D' || blk[3] != '0' || blk[4] != '0' || blk[5] != '1') return 0;
    for (int i = 0; sig[i]; i++) if (blk[7 + i] != (u8)sig[i]) return 0;
    u32 cat = st_le32(blk + 0x47);
    if (!live_read_block(d, cat, blk)) return 0;
    if (blk[0] != 1 || blk[30] != 0x55 || blk[31] != 0xAA) return 0;  /* the validation entry */
    if (blk[32] != 0x88) return 0;                                    /* the default entry must be bootable */
    return st_le32(blk + 32 + 8);                                     /* its load RBA: the image's first block */
}

/* Called once at boot, after the heap exists, when no device on any bus carried our boot sector. 1 = this is now a live boot. */
static int live_try(void) {
    const u8 *mem = MW_BOOTSECT_COPY;
    u32 magic = st_le32(mem + LIVE_FIELD_OFF), sectors = st_le32(mem + LIVE_FIELD_OFF + 4);
    if (magic != LIVE_MAGIC || sectors < 2048u || sectors > LIVE_MAX_SECTORS) return 0;    /* not stamped as a live image */
    for (int d = BLK_CD_FIRST; d < BLK_DEV_MAX; d++) {
        if (!st_devs[d].present) continue;
        u32 image = live_find_image(d);
        if (!image) continue;
        u8 first[512];
        if (!blk_read(d, image * 4u, first) || !st_boot_sector_matches(first)) continue;   /* some other bootable disc */
        u8 *ram = (u8 *)kzalloc(sectors * 512u);
        if (!ram) { serial_puts("[LIVE] not enough memory for the disc image\n"); return 0; }
        serial_puts("[LIVE] copying the disc image into RAM\n");
        for (u32 s = 0; s < sectors; s++) {
            if (!blk_read(d, image * 4u + s, ram + s * 512u)) { serial_puts("[LIVE] read error\n"); kfree(ram); return 0; }
        }
        blk_ram = ram; blk_ram_sectors = sectors;
        st_dev_t *r = &st_devs[BLK_RAM_DEV];
        r->present = 1; r->sectors = sectors;
        const char *m = "RAM disk (live disc)"; int k = 0;
        for (; m[k]; k++) r->model[k] = m[k];
        r->model[k] = 0;
        blk_boot_dev = BLK_RAM_DEV; blk_boot_base = 0; st_boot_id = 0; st_boot_found = 1;
        live_boot = 1;
        serial_puts("[LIVE] running from RAM\n");
        return 1;
    }
    serial_puts("[LIVE] stamped as a live image, but no disc carries it\n");
    return 0;
}

#endif
