/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_PAGING_H
#define MW_PAGING_H

/* paging.h -- x86 paging for the kernel (1.1-alpha-11).
 *
 * WHAT IT DOES TODAY. The CPU is switched to paged addressing with an IDENTITY map (virtual address = physical address), so no kernel
 * pointer, DMA buffer or device register changes -- but the pages are no longer all alike:
 *   - page 0 is NOT PRESENT: a null-pointer read or write (in the kernel or in a program) is a page fault, not a silent access to the
 *     BIOS data at 0;
 *   - the kernel's own code and constants (0x10000 up to `_ro_end`, kernel/link.ld) are READ-ONLY (CR0.WP makes ring 0 obey it too);
 *   - guard pages -- present in the address space but not mapped -- sit under the kernel stack and under the stack of every program that
 *     runs in a window, so a runaway recursion faults at the edge of the stack instead of eating whatever lies below it;
 *   - those faults are survivable: a program in a window that faults is stopped, its window closed, and the desktop carries on
 *     (apps/progwin.h's hook into cpu_fault, fatal.h). A stack overflow needs a stack to report itself on, which is exactly what has
 *     run out -- so a double fault (vector 8) is delivered through a TASK GATE to a TSS with a stack of its own.
 *
 * HOW THE MAP IS BUILT. RAM up to 1 GB is mapped with 4 KB pages (so single pages can be guarded or made read-only); everything else up
 * to 4 GB -- RAM beyond 1 GB, the video frame buffer, device registers (PCI BARs) -- with 4 MB pages (PSE), present and writable, with
 * the cache attributes left to the MTRRs as before paging. The page directory and tables come from mm.h (about 128 KB for 128 MB of RAM).
 *
 * WHAT IT DOES NOT DO (yet), plainly: every page is still supervisor-only and every program still runs in ring 0, so a program can write
 * anywhere that is writable -- the kernel's data, other windows' memory, the page tables themselves. Isolation needs ring 3: user
 * segments and a TSS (this file already builds the GDT and a main TSS), user/supervisor bits on exactly the pages a program owns, and
 * `int 0x80` gates in front of the syscall table. That is the next step; nothing here has to be undone for it.
 *
 * Needs: mm.h (page allocation), fatal.h (the IDT: gate 8 is replaced by the task gate), memmap.h (the stack). */

#define PG_P   1u
#define PG_RW  2u
#define PG_US  4u
#define PG_PS  0x80u
#define PG_4K_LIMIT 0x40000000u          /* RAM below 1 GB gets 4 KB pages */

extern u8 _ro_end[];                     /* kernel/link.ld: end of the kernel's code + constants, page aligned */

static u32 *pg_dir = 0;
static int  pg_enabled = 0;
static u32  pg_tables = 0, pg_guards = 0, pg_ro_pages = 0, pg_ram_top = 0;

static inline void pg_invlpg(u32 a) { __asm__ volatile ("invlpg (%0)" : : "r"(a) : "memory"); }
static inline u32 *pg_pte(u32 a) {
    if (!pg_dir) return 0;
    u32 pde = pg_dir[a >> 22];
    if (!(pde & PG_P) || (pde & PG_PS)) return 0;                  /* a 4 MB page has no table to edit */
    return (u32 *)(pde & ~0xFFFu) + ((a >> 12) & 1023u);
}

/* Makes the 4 KB page at `addr` inaccessible (any touch faults). 1 = done; 0 = not possible (paging off, or the address is in a 4 MB page). */
static int paging_guard(void *addr) {
    u32 a = (u32)(unsigned long)addr & ~0xFFFu;
    u32 *e = pg_enabled ? pg_pte(a) : 0;
    if (!e) return 0;
    if (*e & PG_P) { *e &= ~PG_P; pg_guards++; pg_invlpg(a); }
    return 1;
}
/* Gives it back (call this BEFORE freeing the memory: mm_page_alloc() clears pages it hands out, which would fault on a guard). */
static void paging_unguard(void *addr) {
    u32 a = (u32)(unsigned long)addr & ~0xFFFu;
    u32 *e = pg_enabled ? pg_pte(a) : 0;
    if (e && !(*e & PG_P)) { *e = a | PG_P | PG_RW; if (pg_guards) pg_guards--; pg_invlpg(a); }
}

/* ---------------------------------------------------------------- the double-fault task */
typedef struct {
    u32 link, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi, es, cs, ss, ds, fs, gs, ldt;
    u16 trap, iomap;
} __attribute__((packed)) pg_tss_t;
static pg_tss_t pg_tss_main, pg_tss_df;
static u32 pg_gdt5[5 * 2];                                         /* null, code, data (copied from stage 2), main TSS (0x18), double-fault TSS (0x20) */
static u8  pg_df_stack[8192] __attribute__((aligned(16)));
#define PG_SEL_TSS_MAIN 0x18u
#define PG_SEL_TSS_DF   0x20u

static void pg_gdt_tss(int index, const void *tss) {
    u32 base = (u32)(unsigned long)tss, limit = (u32)sizeof(pg_tss_t) - 1;
    pg_gdt5[index * 2]     = (limit & 0xFFFFu) | ((base & 0xFFFFu) << 16);
    pg_gdt5[index * 2 + 1] = ((base >> 16) & 0xFFu) | (0x89u << 8) | (limit & 0xF0000u) | (base & 0xFF000000u);   /* present, ring 0, 32-bit TSS (available) */
}

static void pg_df_entered(void) __attribute__((noreturn));
static void pg_df_entered(void) {
    u32 fl;                                                         /* we got here by a task switch: the NT flag is set -- clear it */
    __asm__ volatile ("pushfl; popl %0" : "=r"(fl));
    fl &= ~0x4000u;
    __asm__ volatile ("pushl %0; popfl" : : "r"(fl) : "cc");
    pg_gdt_tss(3, &pg_tss_main); pg_gdt_tss(4, &pg_tss_df);          /* both TSS descriptors available again ... */
    __asm__ volatile ("ltr %%ax" : : "a"((u16)PG_SEL_TSS_MAIN));     /* ... and TR back on the main one, so the next double fault can be taken */
    u32 f[13];                                                      /* what the faulting code was doing, in the layout cpu_fault() reads */
    f[0] = pg_tss_main.edi; f[1] = pg_tss_main.esi; f[2] = pg_tss_main.ebp; f[3] = pg_tss_main.esp; f[4] = pg_tss_main.ebx;
    f[5] = pg_tss_main.edx; f[6] = pg_tss_main.ecx; f[7] = pg_tss_main.eax; f[8] = 8; f[9] = 0;
    f[10] = pg_tss_main.eip; f[11] = pg_tss_main.cs; f[12] = pg_tss_main.eflags;
    cpu_fault(f);                                                   /* a program in a window is stopped there; otherwise the error screen */
    for (;;) { }
}

/* ---------------------------------------------------------------- switching paging on */
static void pg_build_gdt(void) {
    struct { u16 limit; u32 base; } __attribute__((packed)) old;
    __asm__ volatile ("sgdt %0" : "=m"(old));
    const u32 *src = (const u32 *)(unsigned long)old.base;
    for (int i = 0; i < 6; i++) pg_gdt5[i] = src[i];                 /* null, code, data: the flat segments stage 2 made */
    pg_tss_main.ss0 = 0x10; pg_tss_main.esp0 = MW_STACK_TOP; pg_tss_main.iomap = (u16)sizeof(pg_tss_t);
    pg_tss_df.eip = (u32)(unsigned long)pg_df_entered; pg_tss_df.eflags = 2;
    pg_tss_df.esp = (u32)(unsigned long)pg_df_stack + sizeof pg_df_stack - 16; pg_tss_df.ss0 = 0x10;
    pg_tss_df.cs = 0x08; pg_tss_df.ss = pg_tss_df.ds = pg_tss_df.es = pg_tss_df.fs = pg_tss_df.gs = 0x10;
    pg_tss_df.cr3 = (u32)(unsigned long)pg_dir; pg_tss_main.cr3 = (u32)(unsigned long)pg_dir; pg_tss_df.iomap = (u16)sizeof(pg_tss_t);
    pg_gdt_tss(3, &pg_tss_main); pg_gdt_tss(4, &pg_tss_df);
    struct { u16 limit; u32 base; } __attribute__((packed)) d = { (u16)(sizeof pg_gdt5 - 1), (u32)(unsigned long)pg_gdt5 };
    __asm__ volatile ("lgdt %0" : : "m"(d));                         /* same flat code/data, two TSS descriptors more: no segment register needs reloading */
    __asm__ volatile ("ltr %%ax" : : "a"((u16)PG_SEL_TSS_MAIN));
    idt_gates[8].lo = (u32)PG_SEL_TSS_DF << 16;                      /* vector 8 = a task gate to the DF TSS: its own stack */
    idt_gates[8].hi = 0x8500u;                                       /* present, ring 0, type 5 (task gate) */
}

/* Builds the tables and switches paging on. Returns 1 if it is on, 0 if it was left off (no PSE, not enough memory): the kernel then runs
 * exactly as before. Call after mm_init_boot() and idt_init(). */
static int paging_init(void) {
    u32 a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    if (!(d & 8u)) return 0;                                         /* no 4 MB pages: not worth building the map two ways */
    u32 kb = *(volatile u32 *)0x9200;
    u32 ram_top = kb >= 0x400000u ? 0xFFFFF000u : kb * 1024u;
    pg_ram_top = ram_top;
    u32 ram4k = ram_top < PG_4K_LIMIT ? ram_top : PG_4K_LIMIT;
    u32 chunks = (ram4k + 0x3FFFFFu) >> 22;                          /* 4 MB chunks with a table of their own (the last one is mapped whole) */
    u32 *dir = (u32 *)mm_page_alloc();
    if (!dir) return 0;
    u32 *tabs[256];
    if (chunks > 256) chunks = 256;
    for (u32 i = 0; i < chunks; i++) {
        tabs[i] = (u32 *)mm_page_alloc();
        if (!tabs[i]) { for (u32 k = 0; k < i; k++) mm_pages_free(tabs[k], 1); mm_pages_free(dir, 1); return 0; }
    }
    u32 ro_end = (u32)(unsigned long)_ro_end, guard_stack = MW_STACK_TOP - MW_STACK_BYTES;
    for (u32 i = 0; i < chunks; i++) {
        for (u32 j = 0; j < 1024; j++) {
            u32 addr = (i * 1024u + j) * 4096u, fl = PG_P | PG_RW;
            if (addr == 0) { tabs[i][j] = 0; continue; }             /* the null page */
            if (addr == guard_stack) { tabs[i][j] = 0; pg_guards++; continue; }      /* under the kernel stack */
            if (addr >= 0x10000u && addr < ro_end) { fl = PG_P; pg_ro_pages++; }      /* kernel code + constants: read-only */
            tabs[i][j] = addr | fl;
        }
        dir[i] = (u32)(unsigned long)tabs[i] | PG_P | PG_RW;
    }
    for (u32 i = chunks; i < 1024; i++) dir[i] = (i << 22) | PG_P | PG_RW | PG_PS;   /* the rest of the 4 GB: 4 MB pages */
    pg_dir = dir; pg_tables = chunks;
    pg_build_gdt();
    __asm__ volatile (
        "movl %%cr4, %%eax\n orl $0x10, %%eax\n movl %%eax, %%cr4\n"           /* PSE */
        "movl %0, %%cr3\n"
        "movl %%cr0, %%eax\n orl $0x80010000, %%eax\n movl %%eax, %%cr0\n"      /* PG | WP */
        "jmp 1f\n1:\n" : : "r"((u32)(unsigned long)dir) : "eax", "memory");
    pg_enabled = 1;
    return 1;
}

#endif
