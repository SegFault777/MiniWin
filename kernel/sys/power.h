/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_SYS_POWER_H
#define MW_SYS_POWER_H
#include "../acpi.h"

/* sys/power.h -- restart / shutdown.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Power management -- as real as a kernel this size can make it. No
 * ACPI beyond the one thing that matters (S5 soft-off, acpi.h), no APM,
 * no drivers: restart is the keyboard-controller trick that worked on bare
 * x86 for decades before any of that existed.
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

/* Real shutdown: paint the classic message first (it is what stays on screen when power cannot be cut, e.g. firmware
 * without ACPI tables), then ask the machine to power off -- ACPI S5, or the hypervisor power-off ports (acpi.h). If that
 * works the screen simply goes dark; if it does not, the message was true and the CPU stops, as it did before 1.1-alpha-5. */
static void system_shutdown(void) {
    __asm__ volatile ("cli");
    bb_fillrect(0, 0, VGA_WIDTH, VGA_HEIGHT, TH_CONSOLE);
    ko_draw_mixed_string(20, 96, t(STR_SAFE_TO_TURN_OFF), TH_CONSOLE_TEXT);
    vga_present();
    acpi_poweroff();
    for (;;) { __asm__ volatile ("hlt"); }
}

#endif
