/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FDC_H
#define FDC_H
#include "io.h"
#include "serial.h"
#include "memmap.h"
#include "delay.h"

/* fdc.h -- the floppy disk controller (an 8272/82077 at I/O ports 0x3F0-0x3F7) and ISA DMA channel 2: one 1.44 MB 3.5" drive.
 *
 * Why a MiniWin kernel has this: virtual machines hand a raw disk image to the guest as a FLOPPY at least as often as
 * as a hard disk (VMware's "floppy image file", VirtualBox's floppy controller), and the BIOS boots it happily -- INT 13h
 * with drive number 00h. The boot loader now reads the kernel from a floppy by CHS (boot/stage2.asm); after that the kernel
 * must reach the SAME floppy by itself, and this is how.
 *
 * Scope on purpose: only the drive the machine was booted from, and only 1.44 MB media (18 sectors per track, 2 heads, 80
 * cylinders) -- exactly what a bootable image is. Polled (interrupts are never enabled), DMA through a fixed buffer.
 * A real floppy turns one revolution per sector it must skip, so the driver reads and caches a WHOLE TRACK (18 sectors) per
 * access: the filesystem's sequential reads then cost one revolution per 18 sectors. The motor is left running (there is no
 * timer to switch it off from); on a virtual floppy that does not matter. Written from the 82077AA datasheet's command set
 * and run against QEMU's emulated controller; VMware/VirtualBox's floppies are untested. */

#ifndef MW_BOOT_INFO8                     /* a byte the boot loader left in low memory (host tests substitute a fake) */
#define MW_BOOT_INFO8(addr) (*(volatile u8 *)(addr))
#endif
#define FD_MAX_DEVS 1
#define FD_SPT      18
#define FD_HEADS    2
#define FD_CYLS     80
#define FD_SECTORS  (FD_SPT * FD_HEADS * FD_CYLS)          /* 2880 */

#define FDC_DOR  0x3F2
#define FDC_MSR  0x3F4
#define FDC_DATA 0x3F5
#define FDC_CCR  0x3F7
#define FD_POLL  400000u

static int fd_dev_count = 0;
static int fd_inited = 0;
static u8  fd_drive = 0;                  /* BIOS drive number of the boot floppy: 0 = A:, 1 = B: */
static int fd_cyl = -1;                   /* the head's current cylinder, -1 = unknown (needs a recalibrate) */
static int fd_motor = 0;
static int fd_cache_track = -1;           /* which track the 9 KB buffer holds, -1 = none */
#define FD_TRACK_BUF ((u8 *)MW_FD_DMA_ADDR)
#define FD_SECT_BUF  ((u8 *)(MW_FD_DMA_ADDR + FD_SPT * 512u))

/* Waits until the controller is ready for a byte in direction `dir` (0 = we write, 1 = we read). */
static inline int fdc_ready(int dir) {
    for (u32 i = 0; i < FD_POLL; i++) {
        u8 m = inb(FDC_MSR);
        if ((m & 0x80) && (((m >> 6) & 1) == dir)) return 1;
    }
    return 0;
}
static inline int fdc_out(u8 b) { if (!fdc_ready(0)) return 0; outb(FDC_DATA, b); return 1; }
static inline int fdc_in(void) { if (!fdc_ready(1)) return -1; return inb(FDC_DATA); }

/* SENSE INTERRUPT STATUS: how a polled driver learns that a seek/recalibrate finished (and clears the controller's latch). */
static inline int fdc_sense(u8 *st0, u8 *pcn) {
    if (!fdc_out(0x08)) return 0;
    int a = fdc_in();
    if (a < 0) return 0;
    *st0 = (u8)a;
    if ((a & 0xC0) == 0x80) { *pcn = 0; return 1; }       /* "invalid": nothing was pending, and no second byte follows */
    int b = fdc_in();
    if (b < 0) return 0;
    *pcn = (u8)b;
    return 1;
}

static inline void fdc_reset(void) {
    outb(FDC_DOR, 0x00);                                  /* hold the controller in reset */
    delay_ms(2);
    outb(FDC_DOR, 0x0C);                                  /* released, DMA/IRQ gate open, no motor, drive 0 */
    delay_ms(10);
    for (int i = 0; i < 4; i++) { u8 a, b; fdc_sense(&a, &b); }   /* a reset leaves four interrupt statuses to drain */
    outb(FDC_CCR, 0x00);                                  /* 500 kbit/s: 1.44 MB media */
    fdc_out(0x03); fdc_out(0xDF); fdc_out(0x02);          /* SPECIFY: step rate 3 ms, head unload 240 ms, head load 2 ms, DMA mode */
    fd_cyl = -1; fd_motor = 0; fd_cache_track = -1;
}

static inline void fdc_motor_on(void) {
    if (fd_motor) return;
    outb(FDC_DOR, (u8)(0x0C | fd_drive | (0x10u << fd_drive)));
    delay_ms(500);                                        /* spin-up */
    fd_motor = 1;
}

/* Waits for a seek or recalibrate to finish (SENSE INTERRUPT shows "seek end"). 1 = done on cylinder `cyl`. */
static inline int fdc_seek_done(int cyl) {
    for (int i = 0; i < 600; i++) {
        u8 st0, pcn;
        delay_ms(2);
        if (fdc_sense(&st0, &pcn) && (st0 & 0xE0) == 0x20 && (cyl < 0 || pcn == cyl)) return 1;
        if ((st0 & 0xC0) == 0x40) return 0;               /* abnormal termination */
    }
    return 0;
}

static inline int fdc_recalibrate(void) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!fdc_out(0x07) || !fdc_out(fd_drive)) return 0;
        if (fdc_seek_done(0)) { fd_cyl = 0; return 1; }
    }
    return 0;
}

static inline int fdc_seek(int cyl) {
    if (fd_cyl < 0 && !fdc_recalibrate()) return 0;
    if (fd_cyl == cyl) return 1;
    if (!fdc_out(0x0F) || !fdc_out(fd_drive) || !fdc_out((u8)cyl)) return 0;
    if (!fdc_seek_done(cyl)) { fd_cyl = -1; return 0; }
    delay_ms(15);                                         /* head settle */
    fd_cyl = cyl;
    return 1;
}

/* ISA DMA channel 2: `len` bytes between memory at `addr` and the controller. `to_mem` = a read from the disk. */
static inline void fdc_dma(u32 addr, u32 len, int to_mem) {
    outb(0x0A, 0x06);                                     /* mask channel 2 */
    outb(0x0C, 0xFF);                                     /* reset the byte flip-flop */
    outb(0x04, (u8)addr); outb(0x04, (u8)(addr >> 8));
    outb(0x81, (u8)(addr >> 16));                         /* page register of channel 2 */
    outb(0x0C, 0xFF);
    outb(0x05, (u8)(len - 1)); outb(0x05, (u8)((len - 1) >> 8));
    outb(0x0B, to_mem ? 0x46 : 0x4A);                     /* single transfer, channel 2, write-to-memory / read-from-memory */
    outb(0x0A, 0x02);                                     /* unmask */
}

/* One READ/WRITE DATA command for `nsec` sectors starting at sector `first` (1-based) of (cyl, head). 1 = completed. */
static inline int fdc_transfer(int cyl, int head, int first, int nsec, int write) {
    MW_BARRIER();
    fdc_dma(write ? (u32)FD_SECT_BUF : (u32)FD_TRACK_BUF, (u32)nsec * 512u, !write);
    u8 cmd[9] = { write ? 0x45 : 0x46, (u8)((head << 2) | fd_drive), (u8)cyl, (u8)head, (u8)first, 2, (u8)(first + nsec - 1), 0x1B, 0xFF };
    for (int i = 0; i < 9; i++) if (!fdc_out(cmd[i])) return 0;
    u8 r[7];
    for (int i = 0; i < 7; i++) { int v = fdc_in(); if (v < 0) return 0; r[i] = (u8)v; }
    MW_BARRIER();
    u8 ic = r[0] & 0xC0;
    if (ic == 0) return 1;
    return ic == 0x40 && r[1] == 0x80 && r[2] == 0;       /* ran to the end of the cylinder: "end of track" is not an error */
}

/* Reads track `track` (= cylinder * 2 + head) into the cache. 1 = ok. */
static inline int fd_load_track(int track) {
    if (fd_cache_track == track) return 1;
    fd_cache_track = -1;
    fdc_motor_on();
    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt == 2) fd_cyl = -1;                    /* the head may have lost its place: recalibrate on the third try */
        if (!fdc_seek(track >> 1)) continue;
        if (fdc_transfer(track >> 1, track & 1, 1, FD_SPT, 0)) { fd_cache_track = track; return 1; }
    }
    return 0;
}

static inline int fd_read(int i, u32 lba, void *buf) {
    if (i != 0 || i >= fd_dev_count || lba >= FD_SECTORS) return 0;
    if (!fd_load_track((int)(lba / FD_SPT))) return 0;
    const u8 *s = FD_TRACK_BUF + (lba % FD_SPT) * 512u; u8 *d = (u8 *)buf;
    for (int k = 0; k < 512; k++) d[k] = s[k];
    return 1;
}

static inline int fd_write(int i, u32 lba, const void *buf) {
    if (i != 0 || i >= fd_dev_count || lba >= FD_SECTORS) return 0;
    int track = (int)(lba / FD_SPT), sect = (int)(lba % FD_SPT);
    u8 *b = FD_SECT_BUF; const u8 *s = (const u8 *)buf;
    for (int k = 0; k < 512; k++) b[k] = s[k];
    fdc_motor_on();
    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt == 2) fd_cyl = -1;
        if (!fdc_seek(track >> 1)) continue;
        if (fdc_transfer(track >> 1, track & 1, sect + 1, 1, 1)) {
            if (fd_cache_track == track) { u8 *c = FD_TRACK_BUF + sect * 512; for (int k = 0; k < 512; k++) c[k] = s[k]; }
            return 1;
        }
    }
    fd_cache_track = -1;
    return 0;
}

/* Registers the floppy the machine booted from (BIOS drive 00h/01h, geometry as the boot loader saw it), if it is a
 * standard 1.44 MB one. Safe to call twice. */
static void fd_init(void) {
    if (fd_inited) return;
    fd_inited = 1;
    u8 drive = MW_BOOT_INFO8(0x0500);                     /* the BIOS boot drive, saved by boot.asm */
    if (drive >= 2) return;                               /* booted from a hard disk: no floppy is touched */
    u8 spt = MW_BOOT_INFO8(0x050C), heads = MW_BOOT_INFO8(0x050D);
    if (spt != FD_SPT || heads != FD_HEADS) {
        serial_puts("[FDC] the boot floppy is not a 1.44 MB disk (BIOS geometry "); serial_put_dec(spt); serial_putc('/'); serial_put_dec(heads);
        serial_puts("): not supported\n");
        return;
    }
    fd_drive = drive;
    fdc_reset();
    fd_dev_count = 1;
    serial_puts("[FDC] fd1 = the boot floppy (drive "); serial_put_dec(drive); serial_puts("), 2880 sectors\n");
}

#endif
