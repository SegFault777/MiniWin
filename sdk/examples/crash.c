/* SPDX-License-Identifier: GPL-3.0-only */
/* @mwp title: CrashTest */
/* @mwp abi: 3 */
/* crash.c -- breaks on purpose, to show what paging (1.1-alpha-11) protects: press a key and the program does something it must not.
 * In a window the OS stops THIS program, closes its window, says why in the status line -- and carries on. (Run full-screen with
 * `run -f`, or on a kernel without paging, and the same keys take the whole machine down: try it in a virtual machine.)
 *   1 write to address 0        2 read from address 0       3 write into the kernel's code
 *   4 runaway recursion (stack overflow)                    5 divide by zero          6 invalid instruction
 * Not covered, and honest about it: a program in ring 0 can still write to the kernel's DATA (that needs ring 3), and a loop that never
 * calls present / key_poll / sleep_ms freezes the desktop (there is no timer interrupt to take the CPU back). */
#include "miniwin.h"

static int recurse(int n) { volatile char pad[400]; pad[0] = (char)n; return recurse(n + 1) + pad[0]; }

void mwp_entry(void) {
    if (!mw_has(3)) return;
    for (;;) {
        mw_fill_screen(mw_ui_color(MW_TH_FACE));
        mw_text(8, 8, "CrashTest: press a key", mw_ui_color(MW_TH_TEXT));
        mw_text(8, 30, "1 write to address 0", mw_ui_color(MW_TH_TEXT));
        mw_text(8, 44, "2 read from address 0", mw_ui_color(MW_TH_TEXT));
        mw_text(8, 58, "3 write into kernel code", mw_ui_color(MW_TH_TEXT));
        mw_text(8, 72, "4 stack overflow", mw_ui_color(MW_TH_TEXT));
        mw_text(8, 86, "5 divide by zero", mw_ui_color(MW_TH_TEXT));
        mw_text(8, 100, "6 invalid instruction", mw_ui_color(MW_TH_TEXT));
        mw_text(8, 122, "ESC quits", mw_ui_color(MW_TH_SHADOW));
        sys.present();
        int k = sys.key_poll();
        volatile unsigned zero = 0, ktext = 0x10000;
        if (k == MW_KEY_ESC) return;
        if (k == '1') *(volatile int *)zero = 1;
        if (k == '2') { volatile int v = *(volatile int *)zero; (void)v; }
        if (k == '3') *(volatile int *)ktext = 0xDEAD;
        if (k == '4') { volatile int r = recurse(0); (void)r; }
        if (k == '5') { volatile int d = 5 / (int)zero; (void)d; }
        if (k == '6') __asm__ volatile ("ud2");
    }
}
