/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SYSINFO_H
#define SYSINFO_H
#include "io.h"
#include "pci.h"
#include "delay.h"
#include "memmap.h"

/* sysinfo.h -- what the machine is, for the Terminal's `fetch`: the CPU's name and speed, how busy the kernel is, the graphics
 * adapter and its video memory, the RAM. Everything here is read from the hardware's own answers (CPUID, the PCI list, the VESA
 * controller block the boot loader fetched, the BIOS memory figure, the TSC against the PIT) -- nothing is invented, and where
 * a machine will not say, the value is reported as unknown instead of guessed.
 *
 * "CPU usage" needs an honest definition, because this kernel never halts the processor (interrupts are not used, the main loop
 * paces itself by spinning in delay()), so the core is, physically, always 100% busy. What does vary is how much of each main-loop
 * pass goes on real work -- input, network, drawing -- versus the pacing spin. That share, averaged over the last ~32 passes using
 * the TSC, is what `fetch` shows as the CPU load; kmain() feeds it through sysinfo_account(). */

static u32 sysinfo_boot_s = 0;                            /* wall-clock seconds-of-day at the end of boot, for the uptime */

/* ---- CPUID / TSC ---- */
static int sysinfo_has_cpuid(void) {
    u32 f1, f2;                                           /* CPUID exists iff EFLAGS.ID (bit 21) can be flipped (see acpi.h) */
    __asm__ volatile ("pushfl; pushfl; popl %0; movl %0, %1; xorl $0x200000, %0; pushl %0; popfl; pushfl; popl %0; popfl"
                      : "=&r"(f1), "=&r"(f2) : : "cc");
    return ((f1 ^ f2) & 0x200000u) != 0;
}
static inline void sysinfo_cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "0"(leaf), "2"(0));
}
static int sysinfo_cpuid_ok = -1;                         /* -1 = not probed yet */
static int sysinfo_tsc_ok = 0;
static void sysinfo_probe(void) {
    if (sysinfo_cpuid_ok >= 0) return;
    sysinfo_cpuid_ok = sysinfo_has_cpuid();
    if (sysinfo_cpuid_ok) {
        u32 a, b, c, d;
        sysinfo_cpuid(1, &a, &b, &c, &d);
        sysinfo_tsc_ok = (d >> 4) & 1;                    /* the TSC feature bit */
    }
}
static inline u32 sysinfo_tsc32(void) {                   /* the low 32 bits of the time-stamp counter (0 where there is none) */
    if (!sysinfo_tsc_ok) return 0;                        /* (RDTSC would fault on such a CPU) */
    u32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    (void)hi;
    return lo;
}

/* Writes the CPU's name into out[49]: the brand string where the CPU has one (CPUID 80000002h-4h), else the vendor string
 * (e.g. "GenuineIntel") with the family number, else "unknown". Leading blanks (Intel pads) are removed. */
static void sysinfo_cpu_name(char *out) {
    sysinfo_probe();
    out[0] = 0;
    if (!sysinfo_cpuid_ok) { const char *u = "unknown (no CPUID)"; for (int i = 0; ; i++) { out[i] = u[i]; if (!u[i]) break; } return; }
    u32 a, b, c, d;
    sysinfo_cpuid(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000004u) {
        char raw[49];
        for (int k = 0; k < 3; k++) {
            sysinfo_cpuid(0x80000002u + (u32)k, &a, &b, &c, &d);
            u32 r[4] = { a, b, c, d };
            for (int j = 0; j < 16; j++) raw[k * 16 + j] = (char)(r[j / 4] >> (8 * (j % 4)));
        }
        raw[48] = 0;
        int s = 0, n = 0;
        while (raw[s] == ' ') s++;
        while (raw[s] && n < 48) out[n++] = raw[s++];
        while (n > 0 && out[n - 1] == ' ') n--;
        out[n] = 0;
        return;
    }
    sysinfo_cpuid(0, &a, &b, &c, &d);                     /* the vendor string is EBX, EDX, ECX */
    u32 v[3] = { b, d, c };
    int n = 0;
    for (int i = 0; i < 12; i++) out[n++] = (char)(v[i / 4] >> (8 * (i % 4)));
    out[n] = 0;
}

/* The TSC frequency in MHz, measured against the PIT over 20 ms (costs those 20 ms). 0 = no TSC. Under a hypervisor this is the
 * speed the virtual CPU runs at, not the host's marketing number. */
static u32 sysinfo_cpu_mhz(void) {
    sysinfo_probe();
    if (!sysinfo_tsc_ok) return 0;
    u32 t0 = sysinfo_tsc32();
    delay_ms(20);
    u32 t1 = sysinfo_tsc32();
    return (t1 - t0) / 20000u;                            /* cycles per 20 ms / 20000 = MHz; no 64-bit division in this kernel */
}

/* ---- CPU load: share of each main-loop pass spent working (see the header comment) ---- */
static u32 sysinfo_work = 0, sysinfo_total = 0;           /* decaying sums, ~32 passes of memory */
static inline void sysinfo_account(u32 work_cycles, u32 idle_cycles) {
    if (work_cycles > 0x03FFFFFFu) work_cycles = 0x03FFFFFFu;     /* a stalled pass must not overflow the sums */
    if (idle_cycles > 0x03FFFFFFu) idle_cycles = 0x03FFFFFFu;
    sysinfo_work  = sysinfo_work  - (sysinfo_work  >> 5) + work_cycles;
    sysinfo_total = sysinfo_total - (sysinfo_total >> 5) + work_cycles + idle_cycles;
}
/* 0..100, or -1 if the machine has no TSC / nothing has been measured yet */
static int sysinfo_cpu_load(void) {
    if (!sysinfo_tsc_ok || sysinfo_total == 0) return -1;
    return (int)(sysinfo_work / (sysinfo_total / 100u + 1u));
}

/* ---- graphics ---- */
/* The VESA controller block the boot loader read with INT 10h AX=4F00h (boot/stage2.asm keeps it at 0x6000). */
#define SYSINFO_VBE_INFO 0x6000u
static int sysinfo_vbe_ok(void) {
    const u8 *p = (const u8 *)SYSINFO_VBE_INFO;
    return p[0] == 'V' && p[1] == 'E' && p[2] == 'S' && p[3] == 'A';
}
/* Video memory in KB as the BIOS reports it (TotalMemory counts 64 KB blocks); 0 = unknown. */
static u32 sysinfo_vram_kb(void) {
    if (!sysinfo_vbe_ok()) return 0;
    return (u32)(*(const u16 *)(SYSINFO_VBE_INFO + 0x12)) * 64u;
}
/* A readable name for the display adapter: the well-known virtual adapters by PCI id, otherwise "<vendor> <device id>" for the
 * first PCI device of class 03 (display), else the VESA OEM string, else "unknown". Writes into out[40]. */
static void sysinfo_gpu_name(char *out) {
    static const char hexd[] = "0123456789ABCDEF";
    const pci_device_t *g = 0;
    for (int i = 0; i < pci_device_count; i++) if (pci_devices[i].class_code == 0x03) { g = &pci_devices[i]; break; }
    const char *name = 0;
    if (g) {
        u32 id = ((u32)g->vendor_id << 16) | g->device_id;
        switch (id) {
            case 0x12341111u: name = "QEMU/Bochs Standard VGA"; break;
            case 0x1B360100u: name = "QEMU QXL"; break;
            case 0x1AF41050u: name = "virtio-gpu"; break;
            case 0x80EEBEEFu: name = "VirtualBox Graphics"; break;
            case 0x15AD0405u: name = "VMware SVGA II"; break;
            default: break;
        }
        if (!name) {
            const char *vn = g->vendor_id == 0x10DE ? "NVIDIA" : g->vendor_id == 0x1002 ? "AMD" : g->vendor_id == 0x8086 ? "Intel" :
                             g->vendor_id == 0x1AF4 ? "virtio" : g->vendor_id == 0x15AD ? "VMware" : g->vendor_id == 0x80EE ? "VirtualBox" : "PCI";
            int n = 0;
            while (*vn && n < 20) out[n++] = *vn++;
            out[n++] = ' ';
            out[n++] = hexd[(g->vendor_id >> 12) & 15]; out[n++] = hexd[(g->vendor_id >> 8) & 15];
            out[n++] = hexd[(g->vendor_id >> 4) & 15];  out[n++] = hexd[g->vendor_id & 15];
            out[n++] = ':';
            out[n++] = hexd[(g->device_id >> 12) & 15]; out[n++] = hexd[(g->device_id >> 8) & 15];
            out[n++] = hexd[(g->device_id >> 4) & 15];  out[n++] = hexd[g->device_id & 15];
            out[n] = 0;
            return;
        }
    }
    if (!name && sysinfo_vbe_ok()) {                      /* the adapter's own name for itself: a far pointer to a C string */
        u32 far = *(const u32 *)(SYSINFO_VBE_INFO + 6);
        const char *oem = (const char *)(((far >> 16) << 4) + (far & 0xFFFFu));
        if (far && (far >> 16) >= 0xC000u) {
            int n = 0;
            while (oem[n] >= 0x20 && oem[n] < 0x7F && n < 39) { out[n] = oem[n]; n++; }
            out[n] = 0;
            if (n) return;
        }
    }
    if (!name) name = "unknown";
    int n = 0;
    while (name[n] && n < 39) { out[n] = name[n]; n++; }
    out[n] = 0;
}

/* ---- memory ---- */
/* The RAM the BIOS reported in KB (boot/stage2.asm's INT 15h E801h probe leaves it at 0x9200); 0 = none. */
static u32 sysinfo_ram_kb(void) { return *(const u32 *)0x9200u; }
/* The RAM the kernel has set aside for itself in KB: everything below the end of the fixed regions (memmap.h). Reserved, not
 * necessarily touched -- there is no allocator to count real use. */
static u32 sysinfo_ram_reserved_kb(void) { return MW_MWP_END / 1024u; }

#endif
