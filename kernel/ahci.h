/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AHCI_H
#define AHCI_H
#include "io.h"
#include "serial.h"
#include "pci.h"
#include "memmap.h"

/* ahci.h -- SATA disks behind an AHCI controller ("SATA in native mode"): the way every modern PC, VirtualBox
 * ("SATA controller"), VMware ("SATA") and QEMU's q35/ich9-ahci present a hard disk. The legacy ATA ports of ata.h
 * cannot see these disks at all, which used to mean MiniWin booted (the BIOS reads the disk for us) and then found
 * no storage whatsoever.
 *
 * What this driver is: polled (no interrupts), one command at a time, one 512-byte sector per command, 48-bit LBA
 * DMA through a bounce sector (so callers' buffers need no alignment), a cache flush after every write (fs.h's
 * crash-safe journal assumes a write that returned 1 is on the platter). What it deliberately is not: NCQ, port
 * multipliers, hot plug, ATAPI, 4K-native disks (refused, with a line on the serial port).
 *
 * The HBA is found by PCI class 01:06:01 and driven through its ABAR (BAR5, memory-mapped). The BIOS has been using
 * it -- INT 13h read this kernel off the disk -- so ports may be running with the BIOS's own command lists in
 * BIOS memory: each port is stopped, pointed at OUR DMA area (memmap.h MW_AHCI_DMA_ADDR) and restarted, after the
 * BIOS/OS ownership hand-off the spec asks for. */

/* The drivers fill DMA structures with ordinary stores and then ring a doorbell with a volatile access (and read results back
 * the same way round): the compiler must not reorder the two, so each hand-off is bracketed with a compiler barrier. x86 itself
 * keeps stores and loads in order for this purpose. */
#ifndef MW_BARRIER
#define MW_BARRIER() __asm__ volatile ("" ::: "memory")
#endif

#define AHCI_MAX_DEVS  ((int)MW_AHCI_DEVS)
typedef struct {
    volatile u8 *hba;      /* the controller's register block (ABAR) */
    u8  port;
    u32 sectors;           /* usable sectors, capped at 2^28 - 1 like the rest of the storage layer */
    char model[41];
} ahci_dev_t;
static ahci_dev_t ahci_devs[AHCI_MAX_DEVS];
static int ahci_dev_count = 0;
static int ahci_inited = 0;

/* HBA registers (offsets from ABAR) */
#define AHCI_CAP    0x00
#define AHCI_GHC    0x04
#define AHCI_PI     0x0C
#define AHCI_CAP2   0x24
#define AHCI_BOHC   0x28
#define AHCI_GHC_AE 0x80000000u
#define AHCI_GHC_IE 0x00000002u
/* port registers (offsets from ABAR + 0x100 + 0x80 * port) */
#define AHCI_P_CLB   0x00
#define AHCI_P_CLBU  0x04
#define AHCI_P_FB    0x08
#define AHCI_P_FBU   0x0C
#define AHCI_P_IS    0x10
#define AHCI_P_IE    0x14
#define AHCI_P_CMD   0x18
#define AHCI_P_TFD   0x20
#define AHCI_P_SIG   0x24
#define AHCI_P_SSTS  0x28
#define AHCI_P_SERR  0x30
#define AHCI_P_CI    0x38
#define AHCI_CMD_ST   0x00000001u
#define AHCI_CMD_CLO  0x00000008u
#define AHCI_CMD_FRE  0x00000010u
#define AHCI_CMD_FR   0x00004000u
#define AHCI_CMD_CR   0x00008000u
#define AHCI_TFD_BUSY 0x00000088u         /* BSY | DRQ */
#define AHCI_TFD_ERR  0x00000001u
#define AHCI_IS_FATAL 0x78000000u         /* HBFS HBDS IFS INFS: the controller or link broke */
#define AHCI_IS_TFES  0x40000000u         /* task file error: the drive said no */
#define AHCI_SIG_ATA  0x00000101u         /* a plain SATA disk (ATAPI is 0xEB140101, port multiplier 0x96690101) */
#define AHCI_POLL     1000000u            /* register reads before a wait gives up (each is an MMIO access: ~1us in a VM) */

static inline u32 ahci_rd(volatile u8 *base, u32 off) { return *(volatile u32 *)(base + off); }
static inline void ahci_wr(volatile u8 *base, u32 off, u32 v) { *(volatile u32 *)(base + off) = v; }
static inline volatile u8 *ahci_port(volatile u8 *hba, u8 port) { return hba + 0x100 + 0x80u * port; }

/* Waits until (reg & mask) == want. 1 = got there, 0 = the poll budget ran out. */
static inline int ahci_wait(volatile u8 *base, u32 off, u32 mask, u32 want) {
    for (u32 i = 0; i < AHCI_POLL; i++) if ((ahci_rd(base, off) & mask) == want) return 1;
    return 0;
}

/* Each device's slice of the DMA area. */
static inline u8 *ahci_dma(int i) { return (u8 *)(MW_AHCI_DMA_ADDR + (u32)i * MW_AHCI_DEV_STRIDE); }
#define AHCI_OFF_FIS    0x400u
#define AHCI_OFF_TABLE  0x500u
#define AHCI_OFF_BOUNCE 0x600u

/* Stops the port's command engine and FIS receiver. 1 = stopped. */
static inline int ahci_port_stop(volatile u8 *p) {
    ahci_wr(p, AHCI_P_CMD, ahci_rd(p, AHCI_P_CMD) & ~(AHCI_CMD_ST | AHCI_CMD_FRE));
    return ahci_wait(p, AHCI_P_CMD, AHCI_CMD_CR | AHCI_CMD_FR, 0);
}

/* Points the port at device slot i's DMA area and starts it. 1 = running. */
static inline int ahci_port_start(volatile u8 *p, int i) {
    u8 *dma = ahci_dma(i);
    for (u32 k = 0; k < 0x600; k++) dma[k] = 0;
    ahci_wr(p, AHCI_P_CLB, (u32)dma);       ahci_wr(p, AHCI_P_CLBU, 0);
    ahci_wr(p, AHCI_P_FB,  (u32)dma + AHCI_OFF_FIS); ahci_wr(p, AHCI_P_FBU, 0);
    ahci_wr(p, AHCI_P_SERR, 0xFFFFFFFFu);
    ahci_wr(p, AHCI_P_IS, 0xFFFFFFFFu);
    ahci_wr(p, AHCI_P_IE, 0);                                    /* polled: nothing may interrupt */
    ahci_wr(p, AHCI_P_CMD, ahci_rd(p, AHCI_P_CMD) | AHCI_CMD_FRE);
    if (ahci_rd(p, AHCI_P_TFD) & AHCI_TFD_BUSY) {                /* a drive stuck busy from the BIOS's last command: ask the HBA to clear it */
        ahci_wr(p, AHCI_P_CMD, ahci_rd(p, AHCI_P_CMD) | AHCI_CMD_CLO);
        ahci_wait(p, AHCI_P_CMD, AHCI_CMD_CLO, 0);
    }
    if (!ahci_wait(p, AHCI_P_TFD, AHCI_TFD_BUSY, 0)) return 0;
    ahci_wr(p, AHCI_P_CMD, ahci_rd(p, AHCI_P_CMD) | AHCI_CMD_ST);
    return 1;
}

/* One command through slot 0. `data` = the bounce sector (or 0 for a command without data). 1 = completed cleanly. */
static inline int ahci_run(int i, u8 command, u32 lba, int write, int with_data) {
    volatile u8 *p = ahci_port(ahci_devs[i].hba, ahci_devs[i].port);
    u8 *dma = ahci_dma(i);
    u8 *table = dma + AHCI_OFF_TABLE;
    u32 *hdr = (u32 *)dma;                                       /* command header 0 */
    for (u32 k = 0; k < 0x90; k++) table[k] = 0;
    table[0] = 0x27; table[1] = 0x80; table[2] = command;        /* host-to-device register FIS, "this is a command" */
    table[4] = (u8)lba; table[5] = (u8)(lba >> 8); table[6] = (u8)(lba >> 16);
    table[7] = 0x40;                                             /* LBA mode */
    table[8] = (u8)(lba >> 24);                                  /* LBA 47:24 -- the upper bytes stay 0 */
    table[12] = (command == 0x25 || command == 0x35) ? 1 : 0;    /* sector count */
    if (with_data) {
        u32 *prd = (u32 *)(table + 0x80);
        prd[0] = (u32)(dma + AHCI_OFF_BOUNCE); prd[1] = 0; prd[2] = 0; prd[3] = 511;   /* one 512-byte entry (the field holds bytes - 1) */
    }
    hdr[0] = 5u | (write ? 0x40u : 0u) | (with_data ? (1u << 16) : 0u);   /* 5-dword FIS, direction, PRD count */
    hdr[1] = 0; hdr[2] = (u32)table; hdr[3] = 0;

    ahci_wr(p, AHCI_P_SERR, 0xFFFFFFFFu);
    ahci_wr(p, AHCI_P_IS, 0xFFFFFFFFu);
    MW_BARRIER();
    ahci_wr(p, AHCI_P_CI, 1);
    for (u32 n = 0; n < AHCI_POLL * 4u; n++) {
        u32 is = ahci_rd(p, AHCI_P_IS);
        if (is & (AHCI_IS_TFES | AHCI_IS_FATAL)) break;
        if (!(ahci_rd(p, AHCI_P_CI) & 1)) { MW_BARRIER(); return (ahci_rd(p, AHCI_P_TFD) & AHCI_TFD_ERR) ? 0 : 1; }
    }
    /* error or silence: stop and restart the port so the next command starts from a known state */
    ahci_port_stop(p);
    ahci_port_start(p, i);
    return 0;
}

static inline int ahci_read(int i, u32 lba, void *buf) {
    if (i < 0 || i >= ahci_dev_count) return 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (ahci_run(i, 0x25, lba, 0, 1)) {                      /* READ DMA EXT */
            const u8 *src = ahci_dma(i) + AHCI_OFF_BOUNCE; u8 *d = (u8 *)buf;
            for (int k = 0; k < 512; k++) d[k] = src[k];
            return 1;
        }
    }
    return 0;
}

static inline int ahci_write(int i, u32 lba, const void *buf) {
    if (i < 0 || i >= ahci_dev_count) return 0;
    u8 *dst = ahci_dma(i) + AHCI_OFF_BOUNCE; const u8 *s = (const u8 *)buf;
    for (int k = 0; k < 512; k++) dst[k] = s[k];
    if (!ahci_run(i, 0x35, lba, 1, 1)) return 0;                 /* WRITE DMA EXT */
    return ahci_run(i, 0xEA, 0, 0, 0);                           /* FLUSH CACHE EXT: the sector is on the platter, not in the drive's cache */
}

/* IDENTIFY DEVICE for the (already started) port of device i. 1 = a usable 512-byte-sector LBA disk. */
static inline int ahci_identify(int i) {
    if (!ahci_run(i, 0xEC, 0, 0, 1)) return 0;
    const u16 *w = (const u16 *)(ahci_dma(i) + AHCI_OFF_BOUNCE);
    if (!(w[49] & 0x0200)) return 0;                             /* no LBA */
    if ((w[106] & 0xC000) == 0x4000 && (w[106] & 0x1000)) {      /* the logical sector size is in words 117-118 */
        u32 words = (u32)w[117] | ((u32)w[118] << 16);
        if (words != 256) { serial_puts("[AHCI] disk with non-512-byte sectors ignored\n"); return 0; }
    }
    u32 total = (u32)w[60] | ((u32)w[61] << 16);
    if (w[83] & 0x0400) {                                        /* 48-bit LBA: words 100-103 */
        if (w[102] | w[103] | w[101]) total = 0x0FFFFFFFu;
        else { u32 t48 = (u32)w[100] | ((u32)w[101] << 16); if (t48) total = t48; }
    }
    if (total > 0x0FFFFFFFu) total = 0x0FFFFFFFu;
    ahci_devs[i].sectors = total;
    int n = 0;
    for (int k = 27; k <= 46; k++) { ahci_devs[i].model[n++] = (char)(w[k] >> 8); ahci_devs[i].model[n++] = (char)(w[k] & 0xFF); }
    while (n > 0 && (ahci_devs[i].model[n - 1] == ' ' || ahci_devs[i].model[n - 1] == 0)) n--;
    ahci_devs[i].model[n] = 0;
    for (int k = 0; k < n; k++) if ((u8)ahci_devs[i].model[k] < 0x20 || (u8)ahci_devs[i].model[k] > 0x7E) ahci_devs[i].model[k] = '?';
    return total != 0;
}

/* Finds every AHCI controller on the PCI bus, takes it over from the BIOS and registers each attached SATA disk.
 * Safe to call twice (the second call does nothing). pci_scan() must have run. */
static void ahci_init(void) {
    if (ahci_inited) return;
    ahci_inited = 1;
    ahci_dev_count = 0;
    for (int c = 0; c < pci_device_count && ahci_dev_count < AHCI_MAX_DEVS; c++) {
        const pci_device_t *pd = &pci_devices[c];
        if (pd->class_code != 0x01 || pd->subclass != 0x06 || pd->prog_if != 0x01) continue;
        u32 bar5 = pci_config_read32(pd->bus, pd->slot, pd->func, 0x24);
        if ((bar5 & 1) || (bar5 & ~0xFu) == 0) { serial_puts("[AHCI] controller without a usable ABAR, skipped\n"); continue; }
        pci_enable_bus_mastering(pd->bus, pd->slot, pd->func);
        volatile u8 *hba = (volatile u8 *)(bar5 & ~0xFu);

        if (ahci_rd(hba, AHCI_CAP2) & 1) {                       /* BIOS/OS handoff supported: take ownership (OOS) and let the BIOS let go */
            u32 bohc = ahci_rd(hba, AHCI_BOHC);
            if (bohc & 1) { ahci_wr(hba, AHCI_BOHC, bohc | 2); ahci_wait(hba, AHCI_BOHC, 1, 0); }
        }
        ahci_wr(hba, AHCI_GHC, (ahci_rd(hba, AHCI_GHC) | AHCI_GHC_AE) & ~AHCI_GHC_IE);
        u32 pi = ahci_rd(hba, AHCI_PI);
        for (u8 port = 0; port < 32 && ahci_dev_count < AHCI_MAX_DEVS; port++) {
            if (!(pi & (1u << port))) continue;
            volatile u8 *p = ahci_port(hba, port);
            u32 ssts = ahci_rd(p, AHCI_P_SSTS);
            if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) continue;     /* nothing there, or the link is asleep */
            if (ahci_rd(p, AHCI_P_SIG) != AHCI_SIG_ATA) continue;            /* ATAPI / port multiplier / enclosure: not a disk */
            int i = ahci_dev_count;
            ahci_devs[i].hba = hba; ahci_devs[i].port = port; ahci_devs[i].sectors = 0; ahci_devs[i].model[0] = 0;
            ahci_dev_count++;                                    /* counted first: ahci_run() validates against it */
            if (!ahci_port_stop(p) || !ahci_port_start(p, i) || !ahci_identify(i)) {
                ahci_dev_count--;
                serial_puts("[AHCI] port "); serial_put_dec(port); serial_puts(": no usable disk\n");
                continue;
            }
            serial_puts("[AHCI] sata"); serial_put_dec((u32)i + 1); serial_puts(" = port "); serial_put_dec(port);
            serial_puts(", "); serial_put_dec(ahci_devs[i].sectors); serial_puts(" sectors, "); serial_puts(ahci_devs[i].model); serial_putc('\n');
        }
    }
}

#endif
