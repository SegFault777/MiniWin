/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MPT_H
#define MPT_H
#include "io.h"
#include "serial.h"
#include "pci.h"
#include "memmap.h"

/* mpt.h -- SCSI disks behind an LSI "Fusion-MPT" controller. That one protocol (Message Passing Interface 1.x) is what
 * VMware's "LSI Logic" (parallel SCSI, PCI 1000:0030) and "LSI Logic SAS" (1000:0054) adapters, VirtualBox's "LsiLogic" /
 * "LsiLogic SAS" controllers and QEMU's `-device mptsas1068` all present, and what the real SCSI/SAS 1030/1064/1068 chips
 * speak. (VMware's BusLogic, its Paravirtual adapter, virtio-scsi and the NCR/Symbios 53C8xx are different hardware: not
 * handled.)
 *
 * The recipe is the one every MPT driver follows: reset the IOC to READY; send IOC Facts, IOC Init and Port Enable through
 * the "doorbell" register (a 16-bit-at-a-time handshake that works before the IOC has any host memory to use); then
 * post SCSI I/O requests as 128-byte frames in host RAM, and collect completions from the reply FIFO. A request that
 * succeeds comes back as a bare "context reply"; anything else comes back as an "address reply" -- a frame the IOC
 * wrote into one of the reply buffers we donated, which has to be handed back.
 *
 * Polled (all interrupts masked), one request at a time, one 512-byte sector per I/O through a bounce buffer. Disks are
 * found by probing SCSI target ids 0-15 at LUN 0 with INQUIRY + READ CAPACITY(10) (so: direct-access devices with
 * 512-byte blocks, capped at 2^28 - 1 sectors like the rest of the storage layer). Written against the MPI 1.5 message
 * layouts and run against QEMU's mptsas1068; not yet run on VMware, VirtualBox or real controllers. */

/* The drivers fill DMA structures with ordinary stores and then ring a doorbell with a volatile access (and read results back
 * the same way round): the compiler must not reorder the two, so each hand-off is bracketed with a compiler barrier. x86 itself
 * keeps stores and loads in order for this purpose. */
#ifndef MW_BARRIER
#define MW_BARRIER() __asm__ volatile ("" ::: "memory")
#endif

#define MPT_MAX_CTRL  ((int)MW_MPT_CTRLS)
#define MPT_MAX_DEVS  8
#define MPT_PROBE_IDS 16
typedef struct {
    volatile u8 *mm;       /* memory-mapped registers (BAR1), or 0 */
    u16 io;                /* port-I/O registers (BAR0) when there is no usable BAR1 */
    u8  max_devices;
    u8 *dma;               /* this controller's 4 KB slice of the storage DMA area */
} mpt_ctrl_t;
typedef struct { u8 ctrl; u8 target; u32 sectors; char model[41]; } mpt_dev_t;
static mpt_ctrl_t mpt_ctrls[MPT_MAX_CTRL];
static int mpt_ctrl_count = 0;
static mpt_dev_t mpt_devs[MPT_MAX_DEVS];
static int mpt_dev_count = 0;
static int mpt_inited = 0;

/* registers */
#define MPT_DOORBELL   0x00
#define MPT_WRSEQ      0x04
#define MPT_DIAG       0x08
#define MPT_INTSTAT    0x30
#define MPT_INTMASK    0x34
#define MPT_REQ_FIFO   0x40
#define MPT_REPLY_FIFO 0x44          /* read: reply post FIFO, write: reply free FIFO */
#define MPT_STATE_RESET 0x0u
#define MPT_STATE_READY 0x1u
#define MPT_STATE_OPER  0x2u
#define MPT_STATE_FAULT 0x4u
#define MPT_HIS_DOORBELL 0x00000001u /* IOC -> host: doorbell interrupt */
#define MPT_HIS_SYS2IOC  0x80000000u /* host -> IOC doorbell word not consumed yet */
#define MPT_POLL 3000000u

static inline u32 mpt_rd(const mpt_ctrl_t *c, u32 off) { return c->mm ? *(volatile u32 *)(c->mm + off) : inl((u16)(c->io + off)); }
static inline void mpt_wr(const mpt_ctrl_t *c, u32 off, u32 v) { if (c->mm) *(volatile u32 *)(c->mm + off) = v; else outl((u16)(c->io + off), v); }
static inline u32 mpt_state(const mpt_ctrl_t *c) { return mpt_rd(c, MPT_DOORBELL) >> 28; }

static inline int mpt_wait_int(const mpt_ctrl_t *c) { for (u32 i = 0; i < MPT_POLL; i++) if (mpt_rd(c, MPT_INTSTAT) & MPT_HIS_DOORBELL) return 1; return 0; }
static inline int mpt_wait_ack(const mpt_ctrl_t *c) { for (u32 i = 0; i < MPT_POLL; i++) if (!(mpt_rd(c, MPT_INTSTAT) & MPT_HIS_SYS2IOC)) return 1; return 0; }
static inline int mpt_wait_state(const mpt_ctrl_t *c, u32 want) { for (u32 i = 0; i < MPT_POLL * 2u; i++) if (mpt_state(c) == want) return 1; return 0; }

/* Brings the IOC to READY: first a message-unit reset through the doorbell, and if that does not take, the heavy way (the
 * diagnostic register's reset bit, unlocked by the 5-key write sequence). */
static inline int mpt_reset(const mpt_ctrl_t *c) {
    mpt_wr(c, MPT_INTSTAT, 0);
    mpt_wr(c, MPT_DOORBELL, 0x40u << 24);                    /* IOC message unit reset */
    if (mpt_wait_state(c, MPT_STATE_READY)) return 1;
    static const u8 keys[5] = { 0x04, 0x0B, 0x02, 0x07, 0x0D };
    mpt_wr(c, MPT_WRSEQ, 0xFF);
    for (int i = 0; i < 5; i++) mpt_wr(c, MPT_WRSEQ, keys[i]);
    mpt_wr(c, MPT_DIAG, mpt_rd(c, MPT_DIAG) | 0x04u);        /* reset adapter */
    int ok = mpt_wait_state(c, MPT_STATE_READY);
    mpt_wr(c, MPT_WRSEQ, 0);                                 /* lock the diagnostic register again */
    return ok;
}

/* Sends `dwords` 32-bit words as a doorbell handshake request and reads the reply (16 bits per doorbell read) into rep[]
 * (at most maxw words kept). Returns the reply length in 16-bit words, 0 on failure. */
static inline int mpt_handshake(const mpt_ctrl_t *c, const u32 *req, int dwords, u16 *rep, int maxw) {
    mpt_wr(c, MPT_INTSTAT, 0);
    mpt_wr(c, MPT_DOORBELL, (0x42u << 24) | ((u32)dwords << 16));
    if (!mpt_wait_int(c)) return 0;
    mpt_wr(c, MPT_INTSTAT, 0);
    if (!mpt_wait_ack(c)) return 0;
    for (int i = 0; i < dwords; i++) {
        mpt_wr(c, MPT_DOORBELL, req[i]);
        if (!mpt_wait_ack(c)) return 0;
    }
    int total = 0;
    for (int n = 0; n < 2; n++) {                            /* the first two words carry MsgLength (in 32-bit units) */
        if (!mpt_wait_int(c)) return 0;
        rep[n] = (u16)(mpt_rd(c, MPT_DOORBELL) & 0xFFFF);
        mpt_wr(c, MPT_INTSTAT, 0);
    }
    total = (int)(rep[1] & 0xFF) * 2;
    for (int n = 2; n < total; n++) {
        if (!mpt_wait_int(c)) return 0;
        u16 w = (u16)(mpt_rd(c, MPT_DOORBELL) & 0xFFFF);
        if (n < maxw) rep[n] = w;
        mpt_wr(c, MPT_INTSTAT, 0);
    }
    if (mpt_wait_int(c)) mpt_wr(c, MPT_INTSTAT, 0);          /* the IOC's closing doorbell interrupt */
    return total;
}

/* ---- this controller's DMA slice: request frame, sense buffer, reply frames, bounce buffer ---- */
#define MPT_OFF_REQ     0x000u     /* one 128-byte request frame */
#define MPT_OFF_SENSE   0x080u     /* 32 bytes */
#define MPT_OFF_REPLY   0x100u     /* MPT_NREPLY frames of 128 bytes */
#define MPT_NREPLY      8
#define MPT_OFF_BOUNCE  0x800u     /* 512 bytes (INQUIRY / READ CAPACITY / one sector) */

/* One SCSI command to (target, LUN 0). `len` bytes through the bounce buffer, `write` = host-to-device.
 * 1 = GOOD (a data underrun with GOOD status counts: INQUIRY may return less than asked). */
static inline int mpt_scsi(const mpt_ctrl_t *c, u8 target, const u8 *cdb, int cdblen, u32 len, int write) {
    u32 *f = (u32 *)(c->dma + MPT_OFF_REQ);
    for (int i = 0; i < 32; i++) f[i] = 0;
    u8 *b = (u8 *)f;
    b[0] = target; b[1] = 0; b[3] = 0x00;                    /* TargetID, Bus, Function = SCSI IO request */
    b[4] = (u8)cdblen; b[5] = 32;                            /* CDB length, sense buffer length */
    f[2] = 0x1u;                                             /* MsgContext: a type-0 context, so success comes back as a bare context reply */
    f[5] = len ? (write ? 0x01000000u : 0x02000000u) : 0u;   /* Control: data direction, simple queue */
    for (int i = 0; i < cdblen && i < 16; i++) b[24 + i] = cdb[i];
    f[10] = len;                                             /* DataLength */
    f[11] = (u32)(c->dma + MPT_OFF_SENSE);                   /* SenseBufferLowAddr */
    /* one simple 32-bit scatter/gather element: last, end of buffer, end of list, direction */
    f[12] = (0x10u | 0x80u | 0x40u | 0x01u | (write ? 0x04u : 0u)) << 24 | len;
    f[13] = len ? (u32)(c->dma + MPT_OFF_BOUNCE) : 0;
    MW_BARRIER();
    mpt_wr(c, MPT_REQ_FIFO, (u32)f);
    for (u32 i = 0; i < MPT_POLL * 8u; i++) {
        u32 r = mpt_rd(c, MPT_REPLY_FIFO);
        if (r == 0xFFFFFFFFu) continue;
        MW_BARRIER();
        if (!(r & 0x80000000u)) return 1;                    /* context reply: the request succeeded */
        u32 frame = (r & 0x7FFFFFFFu) << 1;                  /* address reply: the IOC wrote a reply frame */
        const u8 *rp = (const u8 *)frame;
        u8 scsi_status = rp[0x0C]; u16 ioc = (u16)(rp[0x0E] | (rp[0x0F] << 8)) & 0x7FFF;
        mpt_wr(c, MPT_REPLY_FIFO, frame);                    /* give the frame back */
        return scsi_status == 0 && (ioc == 0 || ioc == 0x45);   /* 0x45 = SCSI data underrun */
    }
    return 0;
}

/* INQUIRY data: vendor in bytes 8-15 and product in 16-31, both space-padded -> "VENDOR PRODUCT" without the padding. */
static inline void mpt_copy_model(const u8 *inq, char *out) {
    int n = 0;
    for (int part = 0; part < 2; part++) {
        int from = part ? 16 : 8, len = part ? 16 : 8;
        while (len > 0 && (inq[from + len - 1] == ' ' || inq[from + len - 1] == 0)) len--;
        if (part && n > 0 && len > 0 && n < 40) out[n++] = ' ';
        for (int i = 0; i < len && n < 40; i++) out[n++] = (char)inq[from + i];
    }
    out[n] = 0;
    for (int i = 0; i < n; i++) if ((u8)out[i] < 0x20 || (u8)out[i] > 0x7E) out[i] = '?';
}

/* Brings controller c from whatever the BIOS left to "operational, port enabled". 1 = ready for SCSI I/O. */
static inline int mpt_bringup(mpt_ctrl_t *c) {
    u16 rep[48];
    u32 msg[6];
    mpt_wr(c, MPT_INTMASK, 0x00000009u);                     /* polled: mask the doorbell and reply interrupts */
    if (!mpt_reset(c)) { serial_puts("[MPT] controller did not reset to READY\n"); return 0; }
    mpt_wr(c, MPT_INTMASK, 0x00000009u);

    msg[0] = 0x03000000u; msg[1] = 0; msg[2] = 0x4D50u;      /* IOC Facts */
    if (mpt_handshake(c, msg, 3, rep, 48) < 0x18 || (rep[7] & 0x7FFF) != 0) { serial_puts("[MPT] IOC Facts failed\n"); return 0; }
    c->max_devices = (u8)(rep[0x17] & 0xFF);
    if (c->max_devices == 0 || c->max_devices > MPT_PROBE_IDS) c->max_devices = MPT_PROBE_IDS;

    msg[0] = 0x02000002u;                                    /* IOC Init: WhoInit = host driver */
    msg[1] = ((u32)c->max_devices << 8) | (1u << 16);        /* MaxDevices, MaxBuses = 1 */
    msg[2] = 0x4D51u; msg[3] = 128u;                         /* ReplyFrameSize */
    msg[4] = 0; msg[5] = 0;                                  /* everything we hand over lives below 4 GB */
    if (mpt_handshake(c, msg, 6, rep, 48) < 8 || (rep[7] & 0x7FFF) != 0 || mpt_state(c) != MPT_STATE_OPER) { serial_puts("[MPT] IOC Init failed\n"); return 0; }

    for (int i = 0; i < MPT_NREPLY; i++) mpt_wr(c, MPT_REPLY_FIFO, (u32)(c->dma + MPT_OFF_REPLY + 128u * (u32)i));

    msg[0] = 0x06000000u; msg[1] = 0; msg[2] = 0x4D52u;      /* Port Enable, port 0 */
    if (mpt_handshake(c, msg, 3, rep, 48) < 8 || (rep[7] & 0x7FFF) != 0) { serial_puts("[MPT] Port Enable failed\n"); return 0; }
    return 1;
}

static inline int mpt_read(int i, u32 lba, void *buf) {
    if (i < 0 || i >= mpt_dev_count) return 0;
    const mpt_ctrl_t *c = &mpt_ctrls[mpt_devs[i].ctrl];
    u8 cdb[10] = { 0x28, 0, (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba, 0, 0, 1, 0 };   /* READ(10), 1 block */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (mpt_scsi(c, mpt_devs[i].target, cdb, 10, 512, 0)) {
            const u8 *s = c->dma + MPT_OFF_BOUNCE; u8 *d = (u8 *)buf;
            for (int k = 0; k < 512; k++) d[k] = s[k];
            return 1;
        }
    }
    return 0;
}

static inline int mpt_write(int i, u32 lba, const void *buf) {
    if (i < 0 || i >= mpt_dev_count) return 0;
    const mpt_ctrl_t *c = &mpt_ctrls[mpt_devs[i].ctrl];
    u8 *d = c->dma + MPT_OFF_BOUNCE; const u8 *s = (const u8 *)buf;
    for (int k = 0; k < 512; k++) d[k] = s[k];
    u8 cdb[10] = { 0x2A, 0, (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba, 0, 0, 1, 0 };   /* WRITE(10), 1 block */
    if (!mpt_scsi(c, mpt_devs[i].target, cdb, 10, 512, 1)) return 0;
    u8 sync[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };                                                 /* SYNCHRONIZE CACHE(10): best effort */
    (void)mpt_scsi(c, mpt_devs[i].target, sync, 10, 0, 0);
    return 1;
}

/* Finds the controllers (PCI vendor 1000, the MPT 1.x device ids), brings each up and registers every direct-access
 * disk with 512-byte blocks. Safe to call twice. pci_scan() must have run. */
static void mpt_init(void) {
    if (mpt_inited) return;
    mpt_inited = 1;
    static const u16 ids[] = { 0x0030, 0x0050, 0x0054, 0x0056, 0x0058 };   /* 1030 SPI, 1064, 1068, 1064E, 1068E */
    for (int p = 0; p < pci_device_count && mpt_ctrl_count < MPT_MAX_CTRL; p++) {
        const pci_device_t *pd = &pci_devices[p];
        int known = 0;
        if (pd->vendor_id == 0x1000) for (unsigned k = 0; k < sizeof ids / sizeof ids[0]; k++) if (pd->device_id == ids[k]) known = 1;
        if (!known) continue;
        u32 bar0 = pci_config_read32(pd->bus, pd->slot, pd->func, 0x10);
        u32 bar1 = pci_config_read32(pd->bus, pd->slot, pd->func, 0x14);
        mpt_ctrl_t *c = &mpt_ctrls[mpt_ctrl_count];
        c->mm = 0; c->io = 0;
        if (!(bar1 & 1) && ((bar1 >> 1) & 3) == 0 && (bar1 & ~0xFu)) c->mm = (volatile u8 *)(bar1 & ~0xFu);
        else if ((bar0 & 1) && (bar0 & ~3u)) c->io = (u16)(bar0 & ~3u);
        else { serial_puts("[MPT] controller without usable registers, skipped\n"); continue; }
        c->dma = (u8 *)(MW_MPT_DMA_ADDR + (u32)mpt_ctrl_count * MW_MPT_CTRL_STRIDE);
        pci_enable_bus_mastering(pd->bus, pd->slot, pd->func);
        if (!mpt_bringup(c)) continue;
        int ci = mpt_ctrl_count++;
        for (u8 t = 0; t < c->max_devices && mpt_dev_count < MPT_MAX_DEVS; t++) {
            u8 tur[6] = { 0, 0, 0, 0, 0, 0 };
            u8 inq[6] = { 0x12, 0, 0, 0, 36, 0 };
            u8 cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
            (void)mpt_scsi(c, t, tur, 6, 0, 0);                  /* the first command after a reset reports UNIT ATTENTION: swallow it */
            (void)mpt_scsi(c, t, tur, 6, 0, 0);
            if (!mpt_scsi(c, t, inq, 6, 36, 0)) continue;        /* nobody there */
            const u8 *id = c->dma + MPT_OFF_BOUNCE;
            if ((id[0] & 0x1F) != 0 || (id[0] >> 5) != 0) continue;   /* not a (connected) direct-access device */
            char model[41]; mpt_copy_model(id, model);
            if (!mpt_scsi(c, t, cap, 10, 8, 0)) continue;
            const u8 *r = c->dma + MPT_OFF_BOUNCE;
            u32 last = ((u32)r[0] << 24) | ((u32)r[1] << 16) | ((u32)r[2] << 8) | r[3];
            u32 bs = ((u32)r[4] << 24) | ((u32)r[5] << 16) | ((u32)r[6] << 8) | r[7];
            if (bs != 512) { serial_puts("[MPT] disk with non-512-byte blocks ignored\n"); continue; }
            u32 sectors = last >= 0x0FFFFFFEu ? 0x0FFFFFFFu : last + 1;
            mpt_dev_t *d = &mpt_devs[mpt_dev_count];
            d->ctrl = (u8)ci; d->target = t; d->sectors = sectors;
            for (int k = 0; k < 41; k++) d->model[k] = model[k];
            mpt_dev_count++;
            serial_puts("[MPT] scsi"); serial_put_dec((u32)mpt_dev_count); serial_puts(" = controller "); serial_put_dec((u32)ci);
            serial_puts(", target "); serial_put_dec(t); serial_puts(", "); serial_put_dec(sectors); serial_puts(" sectors, "); serial_puts(model); serial_putc('\n');
        }
    }
}

#endif
