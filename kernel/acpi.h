/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ACPI_H
#define ACPI_H
#include "io.h"
#include "serial.h"

/* acpi.h -- exactly one thing from ACPI: turning the machine off (the "S5" soft-off sleep state). Before this a shutdown
 * painted "It's now safe to turn off your computer." and stopped the CPU -- honest, but in a virtual machine (or on any
 * ATX board) the power stays on and the person has to close the window / hold the button themselves.
 *
 * How S5 works, minimally: firmware leaves an RSDP in low memory -> it points at the RSDT -> the RSDT lists the tables, one
 * of which is the FADT ("FACP"). The FADT names the power-management control register (PM1a_CNT) and the DSDT. The value
 * that means "sleep type S5" is not in a register table but in the DSDT's AML bytecode, as `Name (_S5, Package () { a, b, ...})`;
 * the usual trick -- used by hobby kernels for decades -- is to find the bytes "_S5_" in the DSDT and read the package's
 * first two elements without an AML interpreter. Writing (SLP_TYP << 10) | SLP_EN (bit 13) to PM1a_CNT (and PM1b_CNT if
 * there is one) then powers off. If the firmware has not switched the chipset into ACPI mode, the FADT's SMI_CMD port plus
 * ACPI_ENABLE value does that first.
 *
 * Fallbacks, only when CPUID says we are under a hypervisor (writing a stray port on real hardware is not something to
 * do blind): the ports QEMU/Bochs (0x604, 0xB004) and VirtualBox (0x4004) wire to "power off".
 * Everything is bounds-checked: a corrupt table makes acpi_poweroff() return, never fault. */

static const u8 *acpi_scan(u32 from, u32 to) {
    for (u32 a = from; a + 20 <= to; a += 16) {
        const u8 *p = (const u8 *)a;
        if (p[0] != 'R' || p[1] != 'S' || p[2] != 'D' || p[3] != ' ' || p[4] != 'P' || p[5] != 'T' || p[6] != 'R' || p[7] != ' ') continue;
        u8 sum = 0;
        for (int i = 0; i < 20; i++) sum = (u8)(sum + p[i]);
        if (sum == 0) return p;
    }
    return 0;
}

static inline int acpi_sig(const u8 *t, const char *s) { return t[0] == (u8)s[0] && t[1] == (u8)s[1] && t[2] == (u8)s[2] && t[3] == (u8)s[3]; }
static inline u32 acpi_u32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline int acpi_table_ok(const u8 *t, const char *sig) {
    if (!acpi_sig(t, sig)) return 0;
    u32 len = acpi_u32(t + 4);
    if (len < 36 || len > 0x100000u) return 0;
    u8 sum = 0;
    for (u32 i = 0; i < len; i++) sum = (u8)(sum + t[i]);
    return sum == 0;
}

/* Reads the (SLP_TYPa, SLP_TYPb) pair out of the DSDT's _S5 package. 1 = found. */
static int acpi_find_s5(const u8 *dsdt, u16 *typa, u16 *typb) {
    u32 len = acpi_u32(dsdt + 4);
    for (u32 i = 36; i + 10 < len; i++) {
        if (dsdt[i] != '_' || dsdt[i + 1] != 'S' || dsdt[i + 2] != '5' || dsdt[i + 3] != '_') continue;
        if (!(dsdt[i - 1] == 0x08 || (dsdt[i - 1] == '\\' && i >= 2 && dsdt[i - 2] == 0x08))) continue;   /* a Name () definition, not a reference */
        const u8 *p = dsdt + i + 4;
        if (*p++ != 0x12) continue;                                   /* PackageOp */
        p += ((*p >> 6) & 3) + 1;                                     /* PkgLength: 1 + (top two bits) bytes */
        p++;                                                          /* NumElements */
        u16 v[2];
        for (int k = 0; k < 2; k++) {
            if (*p == 0x0A) p++;                                      /* BytePrefix: the next byte is the value */
            v[k] = *p++;                                              /* (ZeroOp / OneOp are the value themselves) */
        }
        *typa = v[0]; *typb = v[1];
        return 1;
    }
    return 0;
}

static int acpi_under_hypervisor(void) {
    u32 f1, f2;                                                       /* CPUID exists iff EFLAGS.ID (bit 21) can be flipped */
    __asm__ volatile ("pushfl; pushfl; popl %0; movl %0, %1; xorl $0x200000, %0; pushl %0; popfl; pushfl; popl %0; popfl"
                      : "=&r"(f1), "=&r"(f2) : : "cc");
    if (!((f1 ^ f2) & 0x200000u)) return 0;
    u32 a = 1, b, c, d;
    __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (c >> 31) & 1;                                             /* "running under a hypervisor" */
}

/* Tries to power the machine off. Returns only if every method failed. */
static void acpi_poweroff(void) {
    /* The BIOS data area (0x40E holds the EBDA segment) is in page 0, which paging.h leaves NOT PRESENT so that a null pointer faults --
     * reading it straight crashed the kernel with a page fault, so `off` never powered anything off (1.1-alpha-11 regression, caught by
     * tools/test/qemu_power.py). Open the page for this one read, then close it again. */
    paging_unguard((void *)0);
    u16 ebda_seg = *(volatile u16 *)0x40E;
    paging_guard((void *)0);
    const u8 *rsdp = 0;
    if (ebda_seg >= 0x8000u && ebda_seg < 0xA000u) rsdp = acpi_scan((u32)ebda_seg << 4, ((u32)ebda_seg << 4) + 1024);
    if (!rsdp) rsdp = acpi_scan(0xE0000u, 0x100000u);
    if (rsdp) {
        const u8 *rsdt = (const u8 *)acpi_u32(rsdp + 16);
        if (rsdt && acpi_table_ok(rsdt, "RSDT")) {
            u32 n = (acpi_u32(rsdt + 4) - 36) / 4;
            for (u32 i = 0; i < n; i++) {
                const u8 *fadt = (const u8 *)acpi_u32(rsdt + 36 + 4 * i);
                if (!fadt || !acpi_table_ok(fadt, "FACP") || acpi_u32(fadt + 4) < 72) continue;
                u32 dsdt_addr = acpi_u32(fadt + 40);
                if (dsdt_addr == 0 && acpi_u32(fadt + 4) >= 148 && acpi_u32(fadt + 144) == 0) dsdt_addr = acpi_u32(fadt + 140);   /* X_DSDT */
                const u8 *dsdt = (const u8 *)dsdt_addr;
                u16 typa, typb;
                u16 pm1a = (u16)acpi_u32(fadt + 64), pm1b = (u16)acpi_u32(fadt + 68);
                if (!dsdt || !pm1a || !acpi_table_ok(dsdt, "DSDT") || !acpi_find_s5(dsdt, &typa, &typb)) continue;
                u16 smi = (u16)acpi_u32(fadt + 48); u8 enable = fadt[52];
                if (!(inw(pm1a) & 1) && smi && enable) {              /* not in ACPI mode yet: ask the firmware (SMI) to switch */
                    outb(smi, enable);
                    for (u32 w = 0; w < 3000000u && !(inw(pm1a) & 1); w++) { }
                }
                serial_puts("[ACPI] S5 power-off\n");
                outw(pm1a, (u16)((typa << 10) | (1u << 13)));
                if (pm1b) outw(pm1b, (u16)((typb << 10) | (1u << 13)));
                for (u32 w = 0; w < 3000000u; w++) { __asm__ volatile ("pause"); }   /* give the chipset a moment to cut power */
            }
        }
    }
    if (acpi_under_hypervisor()) {
        serial_puts("[ACPI] falling back to hypervisor power-off ports\n");
        outw(0x604, 0x2000);       /* QEMU (PIIX4 PM / ICH9 PM) */
        outw(0xB004, 0x2000);      /* Bochs and older QEMU */
        outw(0x4004, 0x3400);      /* VirtualBox */
    }
}

#endif
