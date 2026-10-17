/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NVME_H
#define NVME_H
#include "io.h"
#include "serial.h"
#include "pci.h"
#include "memmap.h"

/* nvme.h -- NVM Express SSDs (PCI class 01:08:02): what every recent laptop and desktop boots from, what VMware 17 gives a
 * new virtual machine by default, and what QEMU's `-device nvme` and VirtualBox's NVMe controller present.
 *
 * The recipe from the NVMe 1.x spec: disable the controller, hand it an admin submission/completion queue pair (plain
 * arrays in RAM, addressed by the bus address of their first byte), enable it, ask it who it is (Identify controller, then
 * Identify namespace 1), and create one I/O queue pair with two admin commands. After that a sector is a 64-byte command
 * in the submission queue, a doorbell write, and a 16-byte completion whose "phase" bit flips every lap round the queue.
 *
 * Polled (interrupts masked), one command in flight, one 512-byte block per command through a 4 KB bounce page (so a
 * data pointer is just PRP1: no PRP lists), a Flush after every write. Namespace 1 only; namespaces formatted with a
 * block size other than 512 bytes are skipped (with a line on serial), and sizes are capped at 2^28 - 1 sectors like
 * the other buses. Written from the NVMe base spec and run against QEMU's `nvme` device; not yet on VMware,
 * VirtualBox or a real SSD. (Real machines also have to boot MiniWin in the first place: that needs a legacy/CSM BIOS,
 * the kernel being loaded through INT 13h.) */

/* The drivers fill DMA structures with ordinary stores and then ring a doorbell with a volatile access (and read results back
 * the same way round): the compiler must not reorder the two, so each hand-off is bracketed with a compiler barrier. x86 itself
 * keeps stores and loads in order for this purpose. */
#ifndef MW_BARRIER
#define MW_BARRIER() __asm__ volatile ("" ::: "memory")
#endif

#define NVME_MAX_CTRL  ((int)MW_NVME_CTRLS)
#define NVME_MAX_DEVS  4
#define NVME_QDEPTH    16
typedef struct {
    volatile u8 *regs;
    u32 stride;               /* doorbell stride in bytes (4 << CAP.DSTRD) */
    u8 *dma;                  /* this controller's six 4 KB pages */
    u16 asq_tail, acq_head, isq_tail, icq_head;
    u8  acq_phase, icq_phase;
    u16 cid;
} nvme_ctrl_t;
typedef struct { u8 ctrl; u32 nsid; u32 sectors; char model[41]; } nvme_dev_t;
static nvme_ctrl_t nvme_ctrls[NVME_MAX_CTRL];
static int nvme_ctrl_count = 0;
static nvme_dev_t nvme_devs[NVME_MAX_DEVS];
static int nvme_dev_count = 0;
static int nvme_inited = 0;

/* controller registers */
#define NVME_CAP    0x00
#define NVME_INTMS  0x0C
#define NVME_CC     0x14
#define NVME_CSTS   0x1C
#define NVME_AQA    0x24
#define NVME_ASQ    0x28
#define NVME_ACQ    0x30
#define NVME_DB     0x1000
#define NVME_POLL   4000000u

/* the six pages */
#define NVME_PG_ASQ 0x0000u
#define NVME_PG_ACQ 0x1000u
#define NVME_PG_ISQ 0x2000u
#define NVME_PG_ICQ 0x3000u
#define NVME_PG_ID  0x4000u       /* Identify data */
#define NVME_PG_BUF 0x5000u       /* one sector of bounce */

static inline u32 nvme_rd(const nvme_ctrl_t *c, u32 off) { return *(volatile u32 *)(c->regs + off); }
static inline void nvme_wr(const nvme_ctrl_t *c, u32 off, u32 v) { *(volatile u32 *)(c->regs + off) = v; }

static inline int nvme_wait_rdy(const nvme_ctrl_t *c, u32 want) {
    for (u32 i = 0; i < NVME_POLL * 4u; i++) {
        u32 st = nvme_rd(c, NVME_CSTS);
        if (st & 2u) return 0;                          /* CFS: controller fatal status */
        if ((st & 1u) == want) return 1;
    }
    return 0;
}

/* Submits one command (opcode, nsid, PRP1, cdw10..12 as given) on the admin (qid 0) or I/O (qid 1) queue and waits for its
 * completion. Returns 1 on success, 0 on error/timeout. `cdw` points at cdw10, cdw11, cdw12 (3 dwords). */
static inline int nvme_cmd(nvme_ctrl_t *c, int qid, u8 opcode, u32 nsid, u32 prp1, const u32 *cdw) {
    u32 *sq = (u32 *)(c->dma + (qid ? NVME_PG_ISQ : NVME_PG_ASQ));
    volatile u32 *cq = (volatile u32 *)(c->dma + (qid ? NVME_PG_ICQ : NVME_PG_ACQ));
    u16 *tail = qid ? &c->isq_tail : &c->asq_tail;
    u16 *head = qid ? &c->icq_head : &c->acq_head;
    u8  *phase = qid ? &c->icq_phase : &c->acq_phase;
    u32 *e = sq + (u32)*tail * 16u;
    for (int i = 0; i < 16; i++) e[i] = 0;
    u16 cid = ++c->cid;
    e[0] = (u32)opcode | ((u32)cid << 16);                 /* opcode, fused = 0, PRP (not SGL), command id */
    e[1] = nsid;
    e[6] = prp1;                                           /* PRP1 (low); high half and PRP2 stay 0 */
    e[10] = cdw[0]; e[11] = cdw[1]; e[12] = cdw[2];
    *tail = (u16)((*tail + 1) % NVME_QDEPTH);
    MW_BARRIER();
    nvme_wr(c, NVME_DB + (u32)(2 * qid) * c->stride, *tail);
    for (u32 i = 0; i < NVME_POLL * 2u; i++) {
        u32 dw3 = cq[(u32)*head * 4u + 3];
        if (((dw3 >> 16) & 1u) != *phase) continue;        /* nothing new in this slot yet */
        MW_BARRIER();
        *head = (u16)((*head + 1) % NVME_QDEPTH);
        if (*head == 0) *phase ^= 1;
        nvme_wr(c, NVME_DB + (u32)(2 * qid + 1) * c->stride, *head);
        return (dw3 >> 17) == 0;                           /* status code type + code, "do not retry" and "more" bits: all must be 0 */
    }
    return 0;
}

static inline int nvme_rw(int i, u32 lba, void *buf, int write) {
    if (i < 0 || i >= nvme_dev_count) return 0;
    nvme_ctrl_t *c = &nvme_ctrls[nvme_devs[i].ctrl];
    u8 *bounce = c->dma + NVME_PG_BUF;
    if (write) { const u8 *s = (const u8 *)buf; for (int k = 0; k < 512; k++) bounce[k] = s[k]; }
    u32 cdw[3] = { lba, 0, 0 };                            /* SLBA low, SLBA high, 0 = one block */
    if (!nvme_cmd(c, 1, write ? 0x01 : 0x02, nvme_devs[i].nsid, (u32)bounce, cdw)) return 0;
    if (write) { u32 none[3] = { 0, 0, 0 }; return nvme_cmd(c, 1, 0x00, nvme_devs[i].nsid, 0, none); }   /* Flush */
    u8 *d = (u8 *)buf; for (int k = 0; k < 512; k++) d[k] = bounce[k];
    return 1;
}
static inline int nvme_read(int i, u32 lba, void *buf) { return nvme_rw(i, lba, buf, 0) || nvme_rw(i, lba, buf, 0); }
static inline int nvme_write(int i, u32 lba, const void *buf) { return nvme_rw(i, lba, (void *)buf, 1); }

/* Brings controller c from reset to "I/O queue pair ready". 1 = ok. */
static inline int nvme_bringup(nvme_ctrl_t *c) {
    u32 cap_hi = nvme_rd(c, NVME_CAP + 4);
    c->stride = 4u << (cap_hi & 0xF);                      /* CAP.DSTRD */
    if (!(cap_hi & (1u << 5))) { serial_puts("[NVMe] controller has no NVM command set\n"); return 0; }
    nvme_wr(c, NVME_INTMS, 0xFFFFFFFFu);                   /* polled */
    if (nvme_rd(c, NVME_CC) & 1u) {                        /* the BIOS left it running: stop it first */
        nvme_wr(c, NVME_CC, nvme_rd(c, NVME_CC) & ~1u);
        if (!nvme_wait_rdy(c, 0)) { serial_puts("[NVMe] controller would not stop\n"); return 0; }
    }
    for (u32 k = 0; k < 0x4000u; k++) c->dma[k] = 0;
    c->asq_tail = c->acq_head = c->isq_tail = c->icq_head = 0;
    c->acq_phase = c->icq_phase = 1; c->cid = 0;
    nvme_wr(c, NVME_AQA, ((u32)(NVME_QDEPTH - 1) << 16) | (u32)(NVME_QDEPTH - 1));
    nvme_wr(c, NVME_ASQ, (u32)(c->dma + NVME_PG_ASQ)); nvme_wr(c, NVME_ASQ + 4, 0);
    nvme_wr(c, NVME_ACQ, (u32)(c->dma + NVME_PG_ACQ)); nvme_wr(c, NVME_ACQ + 4, 0);
    nvme_wr(c, NVME_CC, 1u | (6u << 16) | (4u << 20));     /* enable; NVM command set, 4 KB pages, 64-byte SQ / 16-byte CQ entries */
    if (!nvme_wait_rdy(c, 1)) { serial_puts("[NVMe] controller did not become ready\n"); return 0; }
    u32 mk_cq[3] = { ((u32)(NVME_QDEPTH - 1) << 16) | 1u, 1u, 0 };           /* queue id 1, size; physically contiguous */
    u32 mk_sq[3] = { ((u32)(NVME_QDEPTH - 1) << 16) | 1u, (1u << 16) | 1u, 0 };   /* queue id 1, completes to CQ 1, contiguous */
    if (!nvme_cmd(c, 0, 0x05, 0, (u32)(c->dma + NVME_PG_ICQ), mk_cq)) { serial_puts("[NVMe] Create I/O CQ failed\n"); return 0; }
    if (!nvme_cmd(c, 0, 0x01, 0, (u32)(c->dma + NVME_PG_ISQ), mk_sq)) { serial_puts("[NVMe] Create I/O SQ failed\n"); return 0; }
    return 1;
}

/* Finds NVMe controllers (class 01:08:02), brings each up and registers namespace 1. Safe to call twice. pci_scan() first. */
static void nvme_init(void) {
    if (nvme_inited) return;
    nvme_inited = 1;
    for (int p = 0; p < pci_device_count && nvme_ctrl_count < NVME_MAX_CTRL; p++) {
        const pci_device_t *pd = &pci_devices[p];
        if (pd->class_code != 0x01 || pd->subclass != 0x08 || pd->prog_if != 0x02) continue;
        u32 bar0 = pci_config_read32(pd->bus, pd->slot, pd->func, 0x10);
        u32 bar1 = pci_config_read32(pd->bus, pd->slot, pd->func, 0x14);
        if ((bar0 & 1) || (bar0 & ~0xFu) == 0 || (((bar0 >> 1) & 3) == 2 && bar1 != 0)) {
            serial_puts("[NVMe] controller registers missing or above 4 GB, skipped\n"); continue;
        }
        nvme_ctrl_t *c = &nvme_ctrls[nvme_ctrl_count];
        c->regs = (volatile u8 *)(bar0 & ~0xFu);
        c->dma = (u8 *)(MW_NVME_DMA_ADDR + (u32)nvme_ctrl_count * MW_NVME_CTRL_STRIDE);
        pci_enable_bus_mastering(pd->bus, pd->slot, pd->func);
        if (!nvme_bringup(c)) continue;
        int ci = nvme_ctrl_count++;
        u32 idc[3] = { 1, 0, 0 };                                              /* Identify, CNS 1 = controller */
        u32 idn[3] = { 0, 0, 0 };                                              /* CNS 0 = namespace */
        u8 *id = c->dma + NVME_PG_ID;
        if (nvme_dev_count >= NVME_MAX_DEVS || !nvme_cmd(c, 0, 0x06, 0, (u32)id, idc)) continue;
        char model[41]; int n = 0;
        for (int k = 24; k < 64 && n < 40; k++) model[n++] = (char)id[k];     /* MN: the 40-byte model number */
        while (n > 0 && (model[n - 1] == ' ' || model[n - 1] == 0)) n--;
        model[n] = 0;
        for (int k = 0; k < n; k++) if ((u8)model[k] < 0x20 || (u8)model[k] > 0x7E) model[k] = '?';
        if (!nvme_cmd(c, 0, 0x06, 1, (u32)id, idn)) { serial_puts("[NVMe] namespace 1 is not available\n"); continue; }
        u32 nsze_lo = (u32)id[0] | ((u32)id[1] << 8) | ((u32)id[2] << 16) | ((u32)id[3] << 24);
        u32 nsze_hi = (u32)id[4] | ((u32)id[5] << 8) | ((u32)id[6] << 16) | ((u32)id[7] << 24);
        u32 lbaf = *(u32 *)(id + 128 + 4u * (id[26] & 0xF));                   /* the format in use: FLBAS selects an LBAF entry */
        if (((lbaf >> 16) & 0xFF) != 9) { serial_puts("[NVMe] namespace with non-512-byte blocks ignored\n"); continue; }
        u32 sectors = (nsze_hi || nsze_lo > 0x0FFFFFFFu) ? 0x0FFFFFFFu : nsze_lo;
        nvme_dev_t *d = &nvme_devs[nvme_dev_count];
        d->ctrl = (u8)ci; d->nsid = 1; d->sectors = sectors;
        for (int k = 0; k < 41; k++) d->model[k] = model[k];
        nvme_dev_count++;
        serial_puts("[NVMe] nvme"); serial_put_dec((u32)nvme_dev_count); serial_puts(" = controller "); serial_put_dec((u32)ci);
        serial_puts(", namespace 1, "); serial_put_dec(sectors); serial_puts(" sectors, "); serial_puts(model); serial_putc('\n');
    }
}

#endif
