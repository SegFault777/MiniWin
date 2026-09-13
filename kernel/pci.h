#ifndef PCI_H
#define PCI_H
#include "io.h"
#include "serial.h"

/* ============================================================
 * PCI configuration space -- the "phone book" every PCI device sits
 * behind. Two ports handle all of it: write a (bus, slot, function,
 * register) address to 0xCF8, then read or write the 32-bit value at
 * 0xCFC. This is the actual mechanism real BIOSes and real OSes use to
 * find hardware; there's no shortcut version.
 * ============================================================ */
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static inline u32 pci_make_address(u8 bus, u8 slot, u8 func, u8 offset) {
    return (u32)0x80000000
         | ((u32)bus << 16)
         | ((u32)slot << 11)
         | ((u32)func << 8)
         | ((u32)offset & 0xFC);
}

static inline u32 pci_config_read32(u8 bus, u8 slot, u8 func, u8 offset) {
    outl(PCI_CONFIG_ADDR, pci_make_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

static inline u16 pci_config_read16(u8 bus, u8 slot, u8 func, u8 offset) {
    u32 v = pci_config_read32(bus, slot, func, offset & 0xFC);
    return (u16)(v >> ((offset & 2) * 8));
}

static inline void pci_config_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 value) {
    outl(PCI_CONFIG_ADDR, pci_make_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

typedef struct {
    u8 bus, slot, func;
    u16 vendor_id, device_id;
    u8 class_code, subclass, prog_if;
    u32 bar0, bar1;
} pci_device_t;

#define PCI_MAX_DEVICES 32
static pci_device_t pci_devices[PCI_MAX_DEVICES];
static int pci_device_count = 0;

/* Network controllers we specifically know how to drive (or plan to).
 * Anything else just gets logged, not touched. */
#define PCI_CLASS_NETWORK 0x02

static const char *pci_class_name(u8 class_code) {
    switch (class_code) {
        case 0x01: return "storage";
        case 0x02: return "network";
        case 0x03: return "display";
        case 0x06: return "bridge";
        case 0x0C: return "serial bus";
        default:   return "other";
    }
}

/* Walks every bus/slot/function looking for anything that answers
 * (vendor id != 0xFFFF means "something is here"). Logs every device it
 * finds over the serial port -- there's no VGA-console equivalent for
 * this kind of raw enumeration dump, and piping it to `-serial file:...`
 * is the standard way hobby OS devs have always debugged exactly this
 * step. Doesn't touch or initialize anything; that's the driver's job,
 * once we know what's actually out there. */
static void pci_scan(void) {
    pci_device_count = 0;
    serial_puts("[PCI] scanning...\n");
    for (u32 bus = 0; bus < 256; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            for (u8 func = 0; func < 8; func++) {
                u16 vendor = pci_config_read16((u8)bus, slot, func, 0x00);
                if (vendor == 0xFFFF) {
                    if (func == 0) break; /* no function 0 -> nothing at this slot at all */
                    continue;
                }
                u16 device = pci_config_read16((u8)bus, slot, func, 0x02);
                u32 classreg = pci_config_read32((u8)bus, slot, func, 0x08);
                u8 class_code = (u8)(classreg >> 24);
                u8 subclass   = (u8)(classreg >> 16);
                u8 prog_if    = (u8)(classreg >> 8);

                serial_puts("[PCI] bus="); serial_put_hex8((u8)bus);
                serial_puts(" slot="); serial_put_hex8(slot);
                serial_puts(" func="); serial_put_hex8(func);
                serial_puts(" vendor="); serial_put_hex16(vendor);
                serial_puts(" device="); serial_put_hex16(device);
                serial_puts(" class="); serial_puts(pci_class_name(class_code));
                serial_puts(" ("); serial_put_hex8(class_code);
                serial_putc(':'); serial_put_hex8(subclass);
                serial_putc(')'); serial_putc('\n');

                if (pci_device_count < PCI_MAX_DEVICES) {
                    pci_device_t *d = &pci_devices[pci_device_count++];
                    d->bus = (u8)bus; d->slot = slot; d->func = func;
                    d->vendor_id = vendor; d->device_id = device;
                    d->class_code = class_code; d->subclass = subclass; d->prog_if = prog_if;
                    d->bar0 = pci_config_read32((u8)bus, slot, func, 0x10);
                    d->bar1 = pci_config_read32((u8)bus, slot, func, 0x14);
                }

                /* single-function devices only report meaningfully at
                 * func 0; bit 7 of the header-type register (0x0E) says
                 * whether it's worth checking func 1-7 at all */
                if (func == 0) {
                    u8 header_type = (u8)(pci_config_read16((u8)bus, slot, 0, 0x0E) & 0xFF);
                    if (!(header_type & 0x80)) break;
                }
            }
        }
    }
    serial_puts("[PCI] scan complete, "); serial_put_hex8((u8)pci_device_count); serial_puts(" device(s)\n");
}

/* Finds the first PCI device of a given class/subclass, or NULL. Used
 * to locate "the network card" without caring which specific chipset
 * QEMU (or real hardware) happens to be presenting. */
static pci_device_t *pci_find_class(u8 class_code, u8 subclass) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].class_code == class_code && pci_devices[i].subclass == subclass) {
            return &pci_devices[i];
        }
    }
    return 0;
}

static pci_device_t *pci_find_device(u16 vendor_id, u16 device_id) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id && pci_devices[i].device_id == device_id) {
            return &pci_devices[i];
        }
    }
    return 0;
}

/* Flips on bit 2 (Bus Master Enable) in the PCI command register, plus
 * I/O-space and memory-space access (bits 0-1) for good measure. Every
 * NIC that does DMA -- which is to say every NIC worth writing a driver
 * for -- needs this set before it's allowed to touch system memory on
 * its own. BIOSes usually leave PCI devices with this off by default. */
static void pci_enable_bus_mastering(u8 bus, u8 slot, u8 func) {
    u32 orig = pci_config_read32(bus, slot, func, 0x04);
    u32 updated = orig | 0x0007; /* I/O space + memory space + bus master */
    pci_config_write32(bus, slot, func, 0x04, updated);
}

#endif
