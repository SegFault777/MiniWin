/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ATA_H
#define ATA_H
#include "io.h"

/* ATA, PIO mode, LBA28. We're talking straight to the disk controller hardware here -- no BIOS calls,
 * because BIOS interrupts stop working the instant we're in 32-bit protected mode and there's no OS
 * underneath to ask nicely for us. QEMU's default `-drive format=raw,file=...` (no explicit if=) shows
 * up as legacy IDE, which is the exact same disk the bootloader's BIOS int 0x13 already read from to
 * get this kernel loaded in the first place. Same disk, different door.
 *
 * Four device POSITIONS exist on a legacy IDE controller pair, and this file can talk to any of them:
 *      0 = primary channel   (I/O 0x1F0, control 0x3F6), master      -> storage id "ide1"
 *      1 = primary channel,                               slave       -> "ide2"
 *      2 = secondary channel (I/O 0x170, control 0x376), master       -> "ide3"
 *      3 = secondary channel,                             slave       -> "ide4"
 * The functions named ata_at_*() take the position explicitly. The plain ata_read_sector() /
 * ata_write_sector() that the rest of the kernel (fs.h, icons, programs) has always used keep their
 * signature: they talk to the BOOT device -- the position this installation was started from -- and add
 * the installation's base LBA (non-zero only when MiniWin lives inside a partition, see storage.h), so
 * every "LBA 1029" in fs.h means "1029 sectors into MY installation" wherever that is. */

#define ATA_POSITIONS 4
typedef struct { u16 io; u16 ctl; u8 slave; } ata_pos_t;
static const ata_pos_t ata_positions[ATA_POSITIONS] = {
    { 0x1F0, 0x3F6, 0 }, { 0x1F0, 0x3F6, 1 }, { 0x170, 0x376, 0 }, { 0x170, 0x376, 1 },
};

/* register offsets from the channel's I/O base */
#define ATA_R_DATA       0
#define ATA_R_ERROR      1
#define ATA_R_SECCOUNT   2
#define ATA_R_LBA_LO     3
#define ATA_R_LBA_MID    4
#define ATA_R_LBA_HI     5
#define ATA_R_DRIVE_HEAD 6
#define ATA_R_STATUS     7
#define ATA_R_COMMAND    7

#define ATA_CMD_READ_SECTORS   0x20
#define ATA_CMD_WRITE_SECTORS  0x30
#define ATA_CMD_CACHE_FLUSH    0xE7
#define ATA_CMD_IDENTIFY       0xEC

#define ATA_SR_BSY  0x80
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01
#define ATA_SR_DF   0x20   /* drive fault: the drive itself says it's broken */

/* How many times we poll a status register before declaring the drive dead. There's no clock this low in
 * the stack (no PIT/RTC yet when the first sectors are read), so the budget is a plain iteration count: an
 * inb() on legacy IDE costs on the order of a microsecond, which makes this roughly half a second to a few
 * seconds depending on the machine -- generously longer than any healthy drive ever stays busy, and still a
 * rounding error next to "forever", which is what this loop used to wait for when a disk went missing
 * mid-command. */
#define ATA_POLL_LIMIT  2000000u

/* A drive needs ~400ns after being selected or sent a command before its status register can be trusted;
 * four reads of the status register is the standard way to burn that time. */
static inline void ata_at_delay(u16 io) {
    for (int i = 0; i < 4; i++) (void)inb(io + ATA_R_STATUS);
}

/* Wait for the drive to shut up about being busy -- but not forever. Returns 1 once BSY is clear, 0 if the
 * drive was still busy when patience ran out. A bus with nothing on it floats to 0xFF (every bit set, BSY
 * included), so a missing disk lands here too instead of hanging the kernel's one-and-only main loop. */
static inline int ata_at_wait_bsy_clear(u16 io) {
    for (u32 i = 0; i < ATA_POLL_LIMIT; i++) {
        if (!(inb(io + ATA_R_STATUS) & ATA_SR_BSY)) return 1;
    }
    return 0;
}

/* Wait for the drive to either say "ready for data" (DRQ) or "nope, something broke" (ERR). Returns 1 for
 * the good outcome, 0 for ERR, a drive fault (DF), or silence until the poll budget runs out. */
static inline int ata_at_wait_drq(u16 io) {
    for (u32 i = 0; i < ATA_POLL_LIMIT; i++) {
        u8 status = inb(io + ATA_R_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return 0;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 1;
    }
    return 0;
}

static inline void ata_at_select_lba(u16 io, u8 slave, u32 lba) {
    /* 0xE0 = "LBA mode, master" (0xF0 for the slave); the top 4 bits of the 28-bit LBA cram into the low
     * nibble of this register because that's just how it is. */
    outb(io + ATA_R_DRIVE_HEAD, (u8)(0xE0 | (slave ? 0x10 : 0) | ((lba >> 24) & 0x0F)));
    ata_at_delay(io);
    outb(io + ATA_R_SECCOUNT, 1);
    outb(io + ATA_R_LBA_LO,  (u8)(lba & 0xFF));
    outb(io + ATA_R_LBA_MID, (u8)((lba >> 8) & 0xFF));
    outb(io + ATA_R_LBA_HI,  (u8)((lba >> 16) & 0xFF));
}

/* Reads one 512-byte sector at ABSOLUTE `lba` of the device at (io, slave). Returns 1 if it worked, 0 if the
 * drive reported an error, a fault, or stopped answering (every status wait is bounded by ATA_POLL_LIMIT). */
static inline int ata_at_read(u16 io, u8 slave, u32 lba, void *buf) {
    for (int attempt = 0; attempt < 4; attempt++) {
        if (!ata_at_wait_bsy_clear(io)) return 0; /* drive never settled -- retrying a dead drive 4x just makes the wait 4x longer */
        ata_at_select_lba(io, slave, lba);
        outb(io + ATA_R_COMMAND, ATA_CMD_READ_SECTORS);
        if (!ata_at_wait_drq(io)) return 0; /* real ERR status (or silence) -- not what the retry is for, fail now */

        u16 *p = (u16*)buf;
        for (int i = 0; i < 256; i++) {
            p[i] = inw(io + ATA_R_DATA);
        }

        if (p[0] != 0 || p[1] != 0) return 1; /* looks like real data */
        /* looked like a zeroed sector -- try once more rather than trust it, unless this was already the last attempt */
    }
    return 1; /* out of retries -- hand back whatever the last attempt read rather than failing outright; a genuinely
               * blank sector (an empty document/program slot, say) is a real, valid thing for a caller to read back,
               * and the callers that care about a nonzero magic number already check for that themselves */
}

/* Writes one 512-byte sector at ABSOLUTE `lba` of the device at (io, slave). Returns 1 on success, 0 on any
 * error/fault/timeout (the old version could only ever say 1 or hang). */
static inline int ata_at_write(u16 io, u8 slave, u32 lba, const void *buf) {
    if (!ata_at_wait_bsy_clear(io)) return 0;
    ata_at_select_lba(io, slave, lba);
    outb(io + ATA_R_COMMAND, ATA_CMD_WRITE_SECTORS);
    if (!ata_at_wait_drq(io)) return 0;

    const u16 *p = (const u16*)buf;
    for (int i = 0; i < 256; i++) {
        outw(io + ATA_R_DATA, p[i]);
    }

    if (!ata_at_wait_bsy_clear(io)) return 0;   /* the sector never finished landing */
    outb(io + ATA_R_COMMAND, ATA_CMD_CACHE_FLUSH);
    if (!ata_at_wait_bsy_clear(io)) return 0;   /* ...or the flush never finished -- either way, don't claim success */
    return (inb(io + ATA_R_STATUS) & (ATA_SR_ERR | ATA_SR_DF)) ? 0 : 1;
}

/* ---- IDENTIFY DEVICE: is anything there, and how big is it? ---- */
typedef struct {
    u32 sectors;        /* usable sectors, capped at what LBA28 can address (2^28 - 1) */
    char model[41];     /* trimmed, NUL-terminated */
} ata_ident_t;

/* Returns 1 and fills *id if a plain ATA disk answers at position `p`. Returns 0 for an empty position, a
 * floating bus, an ATAPI device (CD/DVD: signature 0x14/0xEB), a SATA device behind an AHCI-mode controller
 * (0x3C/0xC3 -- not reachable through these legacy ports), or a disk without LBA support. */
static inline int ata_at_identify(const ata_pos_t *p, ata_ident_t *id) {
    u16 io = p->io;
    outb(io + ATA_R_DRIVE_HEAD, (u8)(0xA0 | (p->slave ? 0x10 : 0)));
    ata_at_delay(io);
    u8 st = inb(io + ATA_R_STATUS);
    if (st == 0xFF || st == 0x00) return 0;                 /* nothing drives the bus / no device selected */
    outb(io + ATA_R_SECCOUNT, 0); outb(io + ATA_R_LBA_LO, 0); outb(io + ATA_R_LBA_MID, 0); outb(io + ATA_R_LBA_HI, 0);
    outb(io + ATA_R_COMMAND, ATA_CMD_IDENTIFY);
    ata_at_delay(io);
    if (inb(io + ATA_R_STATUS) == 0) return 0;              /* the drive does not exist */
    if (!ata_at_wait_bsy_clear(io)) return 0;
    if (inb(io + ATA_R_LBA_MID) != 0 || inb(io + ATA_R_LBA_HI) != 0) return 0;   /* ATAPI / SATA signature: not ours */
    if (!ata_at_wait_drq(io)) return 0;

    u16 w[256];
    for (int i = 0; i < 256; i++) w[i] = inw(io + ATA_R_DATA);
    if (!(w[49] & 0x0200)) return 0;                        /* no LBA support */

    u32 total = (u32)w[60] | ((u32)w[61] << 16);
    if ((w[83] & 0x0400) && (w[102] | w[103] | w[101])) total = 0x0FFFFFFFu;     /* 48-bit disk bigger than 2^32 sectors */
    else if (w[83] & 0x0400) { u32 t48 = (u32)w[100] | ((u32)w[101] << 16); if (t48) total = t48; }
    if (total > 0x0FFFFFFFu) total = 0x0FFFFFFFu;           /* all this driver can address (LBA28) */
    id->sectors = total;

    int n = 0;
    for (int i = 27; i <= 46; i++) { id->model[n++] = (char)(w[i] >> 8); id->model[n++] = (char)(w[i] & 0xFF); }
    while (n > 0 && (id->model[n - 1] == ' ' || id->model[n - 1] == 0)) n--;
    id->model[n] = 0;
    for (int i = 0; i < n; i++) if ((u8)id->model[i] < 0x20 || (u8)id->model[i] > 0x7E) id->model[i] = '?';
    return total != 0;
}

/* ---- The BOOT device: what the rest of the kernel has always meant by "the disk" ---- */
static int ata_boot_pos = 0;       /* index into ata_positions[]: the device this installation runs from */
static u32 ata_boot_base = 0;      /* first sector of this installation on that device (0 = it owns the disk) */

static inline int ata_read_sector(u32 lba, void *buf) {
    const ata_pos_t *p = &ata_positions[ata_boot_pos];
    return ata_at_read(p->io, p->slave, ata_boot_base + lba, buf);
}

static inline int ata_write_sector(u32 lba, const void *buf) {
    const ata_pos_t *p = &ata_positions[ata_boot_pos];
    return ata_at_write(p->io, p->slave, ata_boot_base + lba, buf);
}

#endif
