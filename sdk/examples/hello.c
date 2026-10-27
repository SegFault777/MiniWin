/* SPDX-License-Identifier: GPL-3.0-only */
/* hello.c -- the smallest useful MiniWin program: the clock, free memory, icons and a heap block. It runs in a window when started with
 * `run HELLO.MWP` (then it draws only the inside) and draws its own frame when started full-screen with `run -f HELLO.MWP`.
 *   sdk/build.sh sdk/examples/hello.c        sdk/install.sh sdk/examples/hello.c HELLO.MWP */
#include "miniwin.h"

void mwp_entry(void) {
    if (!mw_has(2)) {                                   /* an older kernel: only the version-1 drawing calls exist */
        mw_fill_screen(MW_BLUE);
        sys.draw_string(20, 20, "NEEDS MINIWIN 1.1-ALPHA-10 OR NEWER", MW_WHITE);
        sys.present(); mw_wait_key(); return;
    }
    char *buf = sys.mem_alloc(64);                      /* zero-filled; freed for us when we return even if we forget */
    int win = mw_has(3) && mw_windowed();               /* in a window the title bar is the OS's: draw only the inside */
    for (;;) {
        int x = ((int)sys.screen_w - 300) / 2, y = ((int)sys.screen_h - 150) / 2;   /* read every frame: a window can be resized */
        if (win) { mw_fill_screen(MW_FACE); y -= 8; }
        else {
            mw_fill_screen(MW_DESKTOP);
            sys.window_frame(x, y, 300, 150, "HELLO.MWP", MWI_PROGRAM);
            sys.fill_rect(x + 1, y + 17, 298, 132, MW_FACE);
        }
        mw_text(x + 12, y + 28, "Hello from MiniWin!", MW_BLACK);

        mw_time_t t; sys.time_now(&t);
        int len = 0; char tmp[12]; buf[0] = 0;
        mw_append(buf, &len, "Time ");
        if (t.hour < 10) mw_append(buf, &len, "0");
        mw_append(buf, &len, mw_itoa(t.hour, tmp)); mw_append(buf, &len, ":");
        if (t.minute < 10) mw_append(buf, &len, "0");
        mw_append(buf, &len, mw_itoa(t.minute, tmp));
        mw_text(x + 12, y + 50, buf, MW_BLACK);

        len = 0; buf[0] = 0;
        mw_append(buf, &len, "Free RAM ");
        mw_append(buf, &len, mw_itoa((int)(sys.mem_free_kb() / 1024u), tmp)); mw_append(buf, &len, " MB");
        mw_text(x + 12, y + 72, buf, MW_BLACK);

        sys.draw_icon16(x + 12, y + 96, MWI_FOLDER); sys.draw_icon16(x + 34, y + 96, MWI_DOC); sys.draw_icon16(x + 56, y + 96, MWI_WARNING);
        mw_text(x + 12, y + 122, "Press ESC", MW_SHADOW);
        sys.present();
        if (sys.key_poll() == MW_KEY_ESC) break;
    }
    sys.mem_free(buf);
}
