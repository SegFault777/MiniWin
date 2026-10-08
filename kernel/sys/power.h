#ifndef MW_SYS_POWER_H
#define MW_SYS_POWER_H

/* sys/power.h -- restart / shutdown.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Power management -- as real as a kernel this size can make it. No
 * ACPI, no APM, no drivers: just the two tricks that actually worked
 * on bare x86 for decades before any of that existed.
 * ============================================================ */

/* Real reboot: pulse the keyboard controller's reset line. The 8042
 * chip has a spare output pin wired straight to the CPU's RESET input --
 * BIOSes and boot sectors have used this exact trick since long before
 * ACPI existed, and it still works in QEMU and on real hardware alike. */
static void system_restart(void) {
    while (inb(0x64) & 0x02) { } /* wait for the input buffer to clear */
    outb(0x64, 0xFE);
    for (;;) { __asm__ volatile ("hlt"); } /* belt-and-suspenders, in case the pulse didn't take */
}

/* Real shutdown, 1990s-honest: this kernel has no ACPI power-off, so
 * rather than fake one, it does exactly what pre-ACPI PCs actually did
 * -- paint the classic message and physically stop the CPU. The
 * message was never a lie on hardware like that; it's not one here
 * either. */
static void system_shutdown(void) {
    __asm__ volatile ("cli");
    bb_fillrect(0, 0, VGA_WIDTH, VGA_HEIGHT, TH_CONSOLE);
    ko_draw_mixed_string(20, 96, t(STR_SAFE_TO_TURN_OFF), TH_CONSOLE_TEXT);
    vga_present();
    for (;;) { __asm__ volatile ("hlt"); }
}

#endif
