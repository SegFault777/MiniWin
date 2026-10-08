#ifndef ATA_H
#define ATA_H
#include "io.h"

/* Primary ATA bus, PIO mode, LBA28. We're talking straight to the disk
 * controller hardware here -- no BIOS calls, because BIOS interrupts stop
 * working the instant we're in 32-bit protected mode and there's no OS
 * underneath to ask nicely for us. QEMU's default
 * `-drive format=raw,file=...` (no explicit if=) shows up as legacy IDE,
 * which is the exact same disk the bootloader's BIOS int 0x13 already
 * read from to get this kernel loaded in the first place. Same disk,
 * different door. */

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECCOUNT    0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

#define ATA_CMD_READ_SECTORS   0x20
#define ATA_CMD_WRITE_SECTORS  0x30
#define ATA_CMD_CACHE_FLUSH    0xE7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01
#define ATA_SR_DF   0x20   /* drive fault: the drive itself says it's broken */

/* How many times we poll a status register before declaring the drive
 * dead. There's no clock this low in the stack (no PIT/RTC yet when the
 * first sectors are read), so the budget is a plain iteration count: an
 * inb() on legacy IDE costs on the order of a microsecond, which makes
 * this roughly half a second to a few seconds depending on the machine
 * -- generously longer than any healthy drive ever stays busy, and
 * still a rounding error next to "forever", which is what this loop
 * used to wait for when a disk went missing mid-command. */
#define ATA_POLL_LIMIT  2000000u

/* The drive's basically saying "hold on, I'm doing a thing." Wait for it
 * to shut up about being busy -- but not forever. Returns 1 once BSY is
 * clear, 0 if the drive was still busy when patience ran out. A bus with
 * nothing on it floats to 0xFF (every bit set, BSY included), so a
 * missing disk lands here too instead of hanging the whole kernel's
 * one-and-only main loop. */
static inline int ata_wait_bsy_clear(void) {
    for (u32 i = 0; i < ATA_POLL_LIMIT; i++) {
        if (!(inb(ATA_STATUS) & ATA_SR_BSY)) return 1;
    }
    return 0;
}

/* Wait for the drive to either say "ready for data" (DRQ) or "nope,
 * something broke" (ERR). Returns 1 for the good outcome, 0 for ERR, a
 * drive fault (DF, bit 5), or silence until the poll budget runs out. */
static inline int ata_wait_drq(void) {
    for (u32 i = 0; i < ATA_POLL_LIMIT; i++) {
        u8 status = inb(ATA_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return 0;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 1;
    }
    return 0;
}

static inline void ata_select_lba(u32 lba) {
    /* 0xE0 = "master drive, and yes, I mean LBA, not that ancient
     * cylinder/head/sector nonsense." Top 4 bits of the 28-bit LBA cram
     * into the low nibble of this register because that's just how it is. */
    outb(ATA_DRIVE_HEAD, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA_LO,  (u8)(lba & 0xFF));
    outb(ATA_LBA_MID, (u8)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI,  (u8)((lba >> 16) & 0xFF));
}

/* Reads one 512-byte sector at `lba` into `buf`. Returns 1 if it worked,
 * 0 if the drive reported an error, a fault, or stopped answering
 * (every status wait is bounded by ATA_POLL_LIMIT -- see above).
 *
 * The retry loop below exists because of a real, reproducible failure
 * found while building kernel/mwp.h's program loader: several
 * back-to-back reads of the exact same sector, still well within a
 * single boot, would occasionally come back with the DRQ handshake
 * reporting success (ata_wait_drq() returned 1) while the 256 words
 * actually latched off ATA_DATA were all zero -- not a hardware BSY/ERR
 * failure this driver already knows how to detect, just silently wrong
 * data. That smells like a QEMU legacy-IDE emulation timing quirk this
 * kernel's PIO loop can trigger by polling faster than a real drive
 * ever would, rather than an actual protocol violation on either side
 * -- but "smells like" isn't a fix, and there was no reliable way found
 * to detect *which specific step* goes wrong from inside this function.
 * What IS reliable: a sector's first 4 bytes being all-zero is
 * essentially never valid data for anything this kernel actually reads
 * (fs.h's FS_MAGIC and PROG_MAGIC both guarantee a non-zero first
 * dword, and a boot-sector/kernel-image read would fail far more
 * obviously than this). So: treat an all-zero first dword as suspect,
 * and just ask again. Real, persistent hardware failures still get
 * caught by ata_wait_drq()'s own ERR check and fail immediately, same
 * as before -- this retry loop only ever fires for the "reported
 * success but the data looks impossible" case. */
static inline int ata_read_sector(u32 lba, void *buf) {
    for (int attempt = 0; attempt < 4; attempt++) {
        if (!ata_wait_bsy_clear()) return 0; /* drive never settled -- retrying a dead drive 4x just makes the wait 4x longer */
        ata_select_lba(lba);
        outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);
        if (!ata_wait_drq()) return 0; /* real ERR status (or silence) -- not what the retry is for, fail now */

        u16 *p = (u16*)buf;
        for (int i = 0; i < 256; i++) {
            p[i] = inw(ATA_DATA);
        }

        if (p[0] != 0 || p[1] != 0) return 1; /* looks like real data */
        /* looked like a zeroed sector -- try once more rather than
         * trust it, unless this was already the last attempt */
    }
    return 1; /* out of retries -- hand back whatever the last attempt
              * read rather than failing outright; a genuinely blank
              * sector (an empty document/program slot, say) is a real,
              * valid thing for a caller to read back, and the callers
              * that care about a nonzero magic number already check
              * for that themselves */
}

/* Writes one 512-byte sector at `lba` from `buf`. Returns 1 on success,
 * 0 on any error/fault/timeout (the old version could only ever say 1 or
 * hang).
 * Flushes the drive's write cache afterward so the data actually lands
 * in the image file for real, instead of vanishing into a volatile
 * cache the moment someone yanks the power (or, more realistically,
 * closes the QEMU window). This is the difference between "persistent
 * storage" and "storage that just got your hopes up." */
static inline int ata_write_sector(u32 lba, const void *buf) {
    if (!ata_wait_bsy_clear()) return 0;
    ata_select_lba(lba);
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);
    if (!ata_wait_drq()) return 0;

    const u16 *p = (const u16*)buf;
    for (int i = 0; i < 256; i++) {
        outw(ATA_DATA, p[i]);
    }

    if (!ata_wait_bsy_clear()) return 0;   /* the sector never finished landing */
    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    if (!ata_wait_bsy_clear()) return 0;   /* ...or the flush never finished -- either way, don't claim success */
    return (inb(ATA_STATUS) & (ATA_SR_ERR | ATA_SR_DF)) ? 0 : 1;
}

#endif
