/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ATAPI_H
#define ATAPI_H
#include "io.h"
#include "serial.h"
#include "ata.h"
#include "delay.h"

/* atapi.h -- CD-ROM / DVD drives on the legacy IDE channels (ATAPI: SCSI commands carried in 12-byte "packets" over the ATA
 * registers). It is what QEMU's `-cdrom`, VMware's and VirtualBox's "IDE CD/DVD" and every PC with an IDE optical drive present.
 *
 * What it is: a READ-ONLY block device, polled (PIO), at the same four positions as ide1..ide4 -- `cd3` is the ATAPI drive at the
 * secondary master, just as `ide3` would be a disk there. A disc has 2048-byte sectors; the storage layer speaks 512, so one disc
 * sector is served as four, with the last one read kept in a one-block cache (sequential 512-byte reads cost one drive access
 * per four). A drive with no disc, or a disc that will not become ready, is not listed. What it is not: a filesystem (ISO 9660 is a
 * later layer), audio, writing, booting from the disc (El Torito), SATA optical drives (those sit behind AHCI and are skipped there). */

#define ATAPI_BLOCK 2048u
typedef struct { int present; u32 sectors; char model[41]; } atapi_dev_t;     /* sectors in 512-byte units */
static atapi_dev_t atapi_devs[ATA_POSITIONS];
static int atapi_inited = 0;
static u8  atapi_cache[ATAPI_BLOCK];
static int atapi_cache_pos = -1;
static u32 atapi_cache_blk = 0;

/* Sends the 12-byte `packet` to the drive at position p and, if `len` > 0, reads up to `len` bytes of answer into `out`.
 * 1 = the command completed without a check condition. */
static inline int atapi_cmd(const ata_pos_t *p, const u8 *packet, u8 *out, u32 len) {
    u16 io = p->io;
    if (!ata_at_wait_bsy_clear(io)) return 0;
    outb(io + ATA_R_DRIVE_HEAD, (u8)(0xA0 | (p->slave ? 0x10 : 0)));
    ata_at_delay(io);
    if (!ata_at_wait_bsy_clear(io)) return 0;
    outb(io + ATA_R_ERROR, 0);                              /* FEATURES: PIO, no DMA/overlap */
    outb(io + ATA_R_LBA_MID, (u8)(ATAPI_BLOCK & 0xFF));     /* the byte count limit per DRQ block */
    outb(io + ATA_R_LBA_HI, (u8)(ATAPI_BLOCK >> 8));
    outb(io + ATA_R_COMMAND, 0xA0);                         /* PACKET */
    ata_at_delay(io);
    if (!ata_at_wait_drq(io)) return 0;
    for (int i = 0; i < 6; i++) outw(io + ATA_R_DATA, (u16)(packet[2 * i] | (packet[2 * i + 1] << 8)));
    u32 got = 0;
    for (;;) {
        if (!ata_at_wait_bsy_clear(io)) return 0;
        u8 st = inb(io + ATA_R_STATUS);
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return 0;        /* a check condition: the caller may ask REQUEST SENSE */
        if (!(st & ATA_SR_DRQ)) return 1;                   /* done */
        u32 n = (u32)inb(io + ATA_R_LBA_MID) | ((u32)inb(io + ATA_R_LBA_HI) << 8);
        for (u32 i = 0; i < n; i += 2) {
            u16 w = inw(io + ATA_R_DATA);
            if (out && got < len) out[got] = (u8)w;
            if (out && got + 1 < len) out[got + 1] = (u8)(w >> 8);
            got += 2;
        }
    }
}

/* Clears a pending "unit attention" (the first command after a media change fails once) and waits for the disc to spin up. */
static inline int atapi_wait_ready(const ata_pos_t *p) {
    static const u8 tur[12] = { 0x00 };
    static const u8 sense[12] = { 0x03, 0, 0, 0, 18, 0 };
    u8 buf[18];
    for (int i = 0; i < 12; i++) {
        if (atapi_cmd(p, tur, 0, 0)) return 1;
        (void)atapi_cmd(p, sense, buf, 18);                 /* REQUEST SENSE acknowledges the condition */
        delay_ms(60);
    }
    return 0;
}

/* Is there an ATAPI drive with a readable disc at position `i`? Fills atapi_devs[i]. */
static inline int atapi_probe(int i) {
    const ata_pos_t *p = &ata_positions[i];
    u16 io = p->io;
    atapi_devs[i].present = 0;
    outb(io + ATA_R_DRIVE_HEAD, (u8)(0xA0 | (p->slave ? 0x10 : 0)));
    ata_at_delay(io);
    u8 st = inb(io + ATA_R_STATUS);
    if (st == 0xFF || st == 0x00) return 0;                 /* nothing on the bus / not selected */
    outb(io + ATA_R_SECCOUNT, 0); outb(io + ATA_R_LBA_LO, 0); outb(io + ATA_R_LBA_MID, 0); outb(io + ATA_R_LBA_HI, 0);
    outb(io + ATA_R_COMMAND, ATA_CMD_IDENTIFY);             /* a disk answers; an ATAPI drive aborts and leaves its signature */
    ata_at_delay(io);
    if (!ata_at_wait_bsy_clear(io)) return 0;
    if (inb(io + ATA_R_LBA_MID) != 0x14 || inb(io + ATA_R_LBA_HI) != 0xEB) return 0;
    outb(io + ATA_R_COMMAND, 0xA1);                         /* IDENTIFY PACKET DEVICE */
    ata_at_delay(io);
    if (!ata_at_wait_drq(io)) return 0;
    u16 w[256];
    for (int k = 0; k < 256; k++) w[k] = inw(io + ATA_R_DATA);
    if (((w[0] >> 8) & 0x1F) != 0x05) return 0;             /* not a CD/DVD-ROM class device (tape, disk bridge, ...) */
    int n = 0;
    char *m = atapi_devs[i].model;
    for (int k = 27; k <= 46; k++) { m[n++] = (char)(w[k] >> 8); m[n++] = (char)(w[k] & 0xFF); }
    while (n > 0 && (m[n - 1] == ' ' || m[n - 1] == 0)) n--;
    m[n] = 0;
    for (int k = 0; k < n; k++) if ((u8)m[k] < 0x20 || (u8)m[k] > 0x7E) m[k] = '?';
    if (!atapi_wait_ready(p)) { serial_puts("[ATAPI] drive at position "); serial_put_dec((u32)i + 1); serial_puts(" has no readable disc\n"); return 0; }
    u8 cap[8];
    static const u8 rc[12] = { 0x25 };                      /* READ CAPACITY(10) */
    if (!atapi_cmd(p, rc, cap, 8)) return 0;
    u32 last = ((u32)cap[0] << 24) | ((u32)cap[1] << 16) | ((u32)cap[2] << 8) | cap[3];
    u32 bs = ((u32)cap[4] << 24) | ((u32)cap[5] << 16) | ((u32)cap[6] << 8) | cap[7];
    if (bs != ATAPI_BLOCK) return 0;                        /* audio/odd media */
    u32 sectors = last >= 0x03FFFFFEu ? 0x0FFFFFFFu : (last + 1u) * 4u;
    atapi_devs[i].sectors = sectors;
    atapi_devs[i].present = 1;
    atapi_cache_pos = -1;
    return 1;
}

/* Reads one 512-byte sector (absolute `lba`, 512-byte units) of the disc in position i. */
static inline int atapi_read(int i, u32 lba, void *buf) {
    if (i < 0 || i >= ATA_POSITIONS || !atapi_devs[i].present || lba >= atapi_devs[i].sectors) return 0;
    u32 blk = lba >> 2;
    if (atapi_cache_pos != i || atapi_cache_blk != blk) {
        u8 pkt[12] = { 0x28, 0, (u8)(blk >> 24), (u8)(blk >> 16), (u8)(blk >> 8), (u8)blk, 0, 0, 1, 0 };   /* READ(10), 1 block */
        atapi_cache_pos = -1;
        int ok = 0;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) ok = atapi_cmd(&ata_positions[i], pkt, atapi_cache, ATAPI_BLOCK);
        if (!ok) return 0;
        atapi_cache_pos = i; atapi_cache_blk = blk;
    }
    const u8 *s = atapi_cache + (lba & 3u) * 512u; u8 *d = (u8 *)buf;
    for (int k = 0; k < 512; k++) d[k] = s[k];
    return 1;
}

/* Finds the optical drives once (the caller has already identified the plain disks: positions with a disk are skipped). */
static void atapi_init(const int *is_disk) {
    if (atapi_inited) return;
    atapi_inited = 1;
    for (int i = 0; i < ATA_POSITIONS; i++) {
        if (is_disk[i]) continue;
        if (atapi_probe(i)) {
            serial_puts("[ATAPI] cd"); serial_put_dec((u32)i + 1); serial_puts(" = "); serial_puts(atapi_devs[i].model);
            serial_puts(", "); serial_put_dec(atapi_devs[i].sectors); serial_puts(" sectors\n");
            /* a read-back check anyone can verify from outside: the byte sums of sectors 0 and 5 (the latter is the second
             * 512-byte quarter of the second disc block, so the 4-to-1 mapping and the block cache are both exercised) */
            static const u32 probe_lba[2] = { 0, 5 };
            for (int k = 0; k < 2; k++) {
                u8 b[512]; u32 sum = 0;
                if (!atapi_read(i, probe_lba[k], b)) { serial_puts("[ATAPI]   sector read failed\n"); break; }
                for (int j = 0; j < 512; j++) sum += b[j];
                serial_puts("[ATAPI]   sector "); serial_put_dec(probe_lba[k]); serial_puts(" byte sum "); serial_put_dec(sum); serial_putc('\n');
            }
        }
    }
}

#endif
