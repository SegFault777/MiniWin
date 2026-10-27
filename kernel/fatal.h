/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FATAL_H
#define FATAL_H

/* fatal.h -- when the kernel cannot continue, say WHY on the screen, by name.
 *
 * Two things live here. fatal_screen() paints a plain full-screen message straight into the video memory (no backbuffer, no
 * windows, nothing that could itself be what broke) and stops. And the CPU exception handlers: before 1.1-alpha-6 the kernel had
 * no interrupt descriptor table at all, so ANY fault -- a bad pointer, an instruction a virtual CPU does not implement --
 * triple-faulted, and a virtual machine just reset or showed a black screen. Now vectors 0-31 land in cpu_fault() and the person
 * sees "MW-K10 CPU_EXCEPTION_..." with the instruction address, which is what makes a crash in someone else's VMware or
 * VirtualBox reportable. Error names and codes: docs/dev/BOOT-PROCESS.md ("Error codes").
 *
 * Everything is also sent to the serial port, which still works when the screen does not. The screen is 640x480 at 11 pixels per
 * character: at most 58 characters per line, so every line below is kept shorter than that. */

#define FATAL_MAX_LINES 16

static void fatal_hex(char *out, u32 v, int digits) {
    for (int i = 0; i < digits; i++) { u32 n = (v >> (4 * (digits - 1 - i))) & 15u; out[i] = (char)(n < 10 ? '0' + n : 'A' + n - 10); }
    out[digits] = 0;
}

/* appends s to the line being built in buf (capacity cap, including the NUL) */
static void fatal_cat(char *buf, int cap, const char *s) {
    int n = 0; while (buf[n]) n++;
    while (*s && n < cap - 1) buf[n++] = *s++;
    buf[n] = 0;
}

/* Full-screen message: lines[0] is the headline. Never returns. */
static void fatal_screen(const char *const *lines, int count) {
    for (int i = 0; i < count; i++) { serial_puts(i ? "        " : "[FATAL] "); serial_puts(lines[i]); serial_putc('\n'); }
    vga_init_display();                                           /* safe to repeat; picks the framebuffer the boot loader set up */
    if (vga_lfb_ptr) {
        u32 bg = ((u32)0x00 << vga_red_shift) | ((u32)0x00 << vga_green_shift) | ((u32)0xAA << vga_blue_shift);   /* classic blue screen */
        u32 fg = ((u32)0xFF << vga_red_shift) | ((u32)0xFF << vga_green_shift) | ((u32)0xFF << vga_blue_shift);
        for (int y = 0; y < VGA_HEIGHT; y++) for (int x = 0; x < VGA_WIDTH; x++) vga_putpixel(x, y, bg);
        int y = 20;
        for (int i = 0; i < count && i < FATAL_MAX_LINES; i++, y += FONT_CELL + 4) {
            int x = 14;
            for (const char *c = lines[i]; *c; c++, x += FONT_CELL) {
                char ch = *c;
                if (ch < FONT_LATIN_FIRST_CP || ch >= FONT_LATIN_FIRST_CP + FONT_LATIN_COUNT) continue;
                const u16 *g = font_latin[(int)(unsigned char)ch - FONT_LATIN_FIRST_CP];
                for (int row = 0; row < FONT_CELL; row++)
                    for (int col = 0; col < FONT_CELL; col++)
                        if (g[row] & (0x8000 >> col)) vga_putpixel(x + col, y + row, fg);
            }
        }
    }
    for (;;) { __asm__ volatile ("cli; hlt"); }
}

/* ---- CPU exceptions ---- */
static const char *const cpu_exc_names[20] = {
    "DIVIDE_ERROR", "DEBUG", "NMI", "BREAKPOINT", "OVERFLOW", "BOUND_RANGE", "INVALID_OPCODE", "DEVICE_NOT_AVAILABLE",
    "DOUBLE_FAULT", "COPROCESSOR_OVERRUN", "INVALID_TSS", "SEGMENT_NOT_PRESENT", "STACK_FAULT", "GENERAL_PROTECTION",
    "PAGE_FAULT", "RESERVED", "X87_FP_ERROR", "ALIGNMENT_CHECK", "MACHINE_CHECK", "SIMD_FP_ERROR",
};

/* The assembly stubs in kentry.asm push the vector and an error code (0 where the CPU gives none), save the registers, and
 * call this with a pointer to the saved frame: edi esi ebp esp ebx edx ecx eax, vector, error, eip, cs, eflags. */
/* Set by apps/progwin.h: called first for every exception. If a program running in a window caused it, the hook stops THAT program and
 * switches back to the desktop -- it never returns in that case. It returns 0 when the fault was the kernel's own. */
static int (*cpu_fault_hook)(const u32 *frame) = 0;

__attribute__((noreturn, used)) void cpu_fault(const u32 *f) {   /* not static: kentry.asm calls it */
    if (cpu_fault_hook) cpu_fault_hook(f);
    u32 cr2; __asm__ volatile ("movl %%cr2, %0" : "=r"(cr2));
    char l[FATAL_MAX_LINES][64];
    const char *p[FATAL_MAX_LINES];
    u32 vec = f[8];
    for (int i = 0; i < FATAL_MAX_LINES; i++) { l[i][0] = 0; p[i] = l[i]; }
    fatal_cat(l[0], 64, "MiniWin has stopped.");
    fatal_cat(l[2], 64, "ERROR MW-K10 CPU_EXCEPTION_");
    fatal_cat(l[2], 64, vec < 20 ? cpu_exc_names[vec] : "OTHER");
    char h[9];
    fatal_cat(l[3], 64, "vector "); fatal_hex(h, vec, 2); fatal_cat(l[3], 64, h);
    fatal_cat(l[3], 64, "  error code "); fatal_hex(h, f[9], 8); fatal_cat(l[3], 64, h);
    fatal_cat(l[4], 64, "EIP "); fatal_hex(h, f[10], 8); fatal_cat(l[4], 64, h);
    fatal_cat(l[4], 64, "  CS "); fatal_hex(h, f[11], 4); fatal_cat(l[4], 64, h);
    fatal_cat(l[4], 64, "  EFLAGS "); fatal_hex(h, f[12], 8); fatal_cat(l[4], 64, h);
    static const char *const rn[7] = { "EAX ", "EBX ", "ECX ", "EDX ", "ESI ", "EDI ", "EBP " };
    static const int ri[7] = { 7, 4, 6, 5, 1, 0, 2 };
    for (int i = 0; i < 7; i++) {
        char *dst = l[5 + (i >> 1)];
        fatal_cat(dst, 64, rn[i]); fatal_hex(h, f[ri[i]], 8); fatal_cat(dst, 64, h); fatal_cat(dst, 64, "  ");
    }
    if (vec == 14) { fatal_cat(l[8], 64, "ADDRESS ACCESSED (CR2) "); fatal_hex(h, cr2, 8); fatal_cat(l[8], 64, h); }
    if (vec == 8) fatal_cat(l[8], 64, "Double fault: a stack overflow is the usual cause");
    fatal_cat(l[10], 64, "The kernel hit an unexpected CPU exception. Restart");
    fatal_cat(l[11], 64, "the machine. If it happens again, report the error");
    fatal_cat(l[12], 64, "name and EIP above (docs/dev/BOOT-PROCESS.md).");
    fatal_screen(p, 13);
    for (;;) { }
}

/* ---- the interrupt descriptor table: 32 exception gates ---- */
extern u32 isr_stub_table[32];
typedef struct { u32 lo, hi; } idt_gate_t;
static idt_gate_t idt_gates[32];

static void idt_init(void) {
    u32 cs;
    __asm__ volatile ("movl %%cs, %0" : "=r"(cs));
    for (int i = 0; i < 32; i++) {
        u32 a = isr_stub_table[i];
        idt_gates[i].lo = (a & 0xFFFFu) | ((cs & 0xFFFFu) << 16);
        idt_gates[i].hi = (a & 0xFFFF0000u) | 0x8E00u;           /* present, ring 0, 32-bit interrupt gate */
    }
    struct { u16 limit; u32 base; } __attribute__((packed)) d = { (u16)(sizeof idt_gates - 1), (u32)idt_gates };
    __asm__ volatile ("lidt %0" : : "m"(d));
}

#endif
