/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef BLK_H
#define BLK_H
#include "ata.h"
#include "ahci.h"
#include "mpt.h"
#include "nvme.h"

/* blk.h -- "the disks": one numbered table of every block device the kernel can drive, whatever bus it sits on, and
 * the read/write that dispatches to the right driver. Above this file nothing cares whether a sector comes through
 * legacy IDE ports (ata.h), an AHCI SATA port (ahci.h) or, later, a SCSI controller: it asks for (device, absolute
 * LBA, 512 bytes).
 *
 * The device index (`d`, also called `pos` in st_target_t for historical reasons) is laid out by type:
 *      0..3    ide   -- by POSITION, always listed (ide1 = primary master ... ide4 = secondary slave)
 *      4..11   sata  -- in discovery order, controller by controller, port by port: sata1, sata2, ...
 *      12..19  scsi  -- LSI Logic / MPT controllers (mpt.h), controller by controller, target id by target id
 *      20..23  nvme  -- NVM Express SSDs (nvme.h), namespace 1 of each controller
 * and a storage id's device number is 1-based within its type ("sata2" = the second AHCI disk found).
 *
 * The BOOT device (blk_boot_dev) is the disk this installation runs from, and ata_read_sector()/ata_write_sector() --
 * the calls fs.h, the icon cache and the program slots have always used -- talk to it, relative to blk_boot_base (the
 * first sector of this installation: non-zero only when MiniWin lives inside a partition, see storage.h). */

typedef enum { ST_IDE = 0, ST_SATA, ST_NVME, ST_SCSI, ST_USB, ST_TYPE_COUNT } st_type_t;

#define BLK_IDE_FIRST   0
#define BLK_SATA_FIRST  (BLK_IDE_FIRST + ATA_POSITIONS)
#define BLK_SCSI_FIRST  (BLK_SATA_FIRST + AHCI_MAX_DEVS)
#define BLK_NVME_FIRST  (BLK_SCSI_FIRST + MPT_MAX_DEVS)
#define BLK_DEV_MAX     (BLK_NVME_FIRST + NVME_MAX_DEVS)

static inline int blk_type_of(int d) { return d < BLK_SATA_FIRST ? ST_IDE : d < BLK_SCSI_FIRST ? ST_SATA : d < BLK_NVME_FIRST ? ST_SCSI : ST_NVME; }
static inline int blk_first_of(int type) { return type == ST_IDE ? BLK_IDE_FIRST : type == ST_SATA ? BLK_SATA_FIRST : type == ST_SCSI ? BLK_SCSI_FIRST : type == ST_NVME ? BLK_NVME_FIRST : -1; }
static inline int blk_count_of(int type) { return type == ST_IDE ? ATA_POSITIONS : type == ST_SATA ? AHCI_MAX_DEVS : type == ST_SCSI ? MPT_MAX_DEVS : type == ST_NVME ? NVME_MAX_DEVS : 0; }
/* storage id (type, 1-based device number) -> device index, or -1 when no such slot can exist */
static inline int blk_index(int type, u32 number) {
    if (number < 1 || number > (u32)blk_count_of(type)) return -1;
    return blk_first_of(type) + (int)number - 1;
}

typedef struct {
    int  present;
    u32  sectors;       /* usable (LBA28-addressable) sectors */
    char model[41];
} st_dev_t;
static st_dev_t st_devs[BLK_DEV_MAX];
static int st_scanned = 0;

/* Absolute-LBA sector I/O on device d. 1 = worked, 0 = failed (bad device, error, timeout). */
static inline int blk_read(int d, u32 lba, void *buf) {
    if (d < 0 || d >= BLK_DEV_MAX) return 0;
    if (d < BLK_SATA_FIRST) return ata_at_read(ata_positions[d].io, ata_positions[d].slave, lba, buf);
    if (d < BLK_SCSI_FIRST) return ahci_read(d - BLK_SATA_FIRST, lba, buf);
    if (d < BLK_NVME_FIRST) return mpt_read(d - BLK_SCSI_FIRST, lba, buf);
    return nvme_read(d - BLK_NVME_FIRST, lba, buf);
}
static inline int blk_write(int d, u32 lba, const void *buf) {
    if (d < 0 || d >= BLK_DEV_MAX) return 0;
    if (d < BLK_SATA_FIRST) return ata_at_write(ata_positions[d].io, ata_positions[d].slave, lba, buf);
    if (d < BLK_SCSI_FIRST) return ahci_write(d - BLK_SATA_FIRST, lba, buf);
    if (d < BLK_NVME_FIRST) return mpt_write(d - BLK_SCSI_FIRST, lba, buf);
    return nvme_write(d - BLK_NVME_FIRST, lba, buf);
}

/* Brings the bus drivers up (once) and fills st_devs[]. Cheap to repeat: IDE is re-identified, the other buses report
 * what their init found. pci_scan() must have run first. */
static inline void st_scan(void) {
    ahci_init();
    mpt_init();
    nvme_init();
    for (int i = 0; i < BLK_DEV_MAX; i++) { st_devs[i].present = 0; st_devs[i].sectors = 0; st_devs[i].model[0] = 0; }
    for (int i = 0; i < ATA_POSITIONS; i++) {
        ata_ident_t id;
        if (ata_at_identify(&ata_positions[i], &id)) {
            st_devs[i].present = 1;
            st_devs[i].sectors = id.sectors;
            for (int k = 0; k < 41; k++) st_devs[i].model[k] = id.model[k];
        }
    }
    for (int i = 0; i < ahci_dev_count; i++) {
        st_dev_t *s = &st_devs[BLK_SATA_FIRST + i];
        s->present = 1; s->sectors = ahci_devs[i].sectors;
        for (int k = 0; k < 41; k++) s->model[k] = ahci_devs[i].model[k];
    }
    for (int i = 0; i < mpt_dev_count; i++) {
        st_dev_t *s = &st_devs[BLK_SCSI_FIRST + i];
        s->present = 1; s->sectors = mpt_devs[i].sectors;
        for (int k = 0; k < 41; k++) s->model[k] = mpt_devs[i].model[k];
    }
    for (int i = 0; i < nvme_dev_count; i++) {
        st_dev_t *s = &st_devs[BLK_NVME_FIRST + i];
        s->present = 1; s->sectors = nvme_devs[i].sectors;
        for (int k = 0; k < 41; k++) s->model[k] = nvme_devs[i].model[k];
    }
    st_scanned = 1;
}

/* ---- the BOOT device ---- */
static int blk_boot_dev = 0;       /* index into st_devs[]: the device this installation runs from (default ide1) */
static u32 blk_boot_base = 0;      /* first sector of this installation on that device (0 = it owns the disk) */

static inline int ata_read_sector(u32 lba, void *buf) { return blk_read(blk_boot_dev, blk_boot_base + lba, buf); }
static inline int ata_write_sector(u32 lba, const void *buf) { return blk_write(blk_boot_dev, blk_boot_base + lba, buf); }

#endif
