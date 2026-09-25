#include "mwp_api.h"

/* GREETER.MWP -- the "hello, world" of MiniWin's loadable-program
 * story. Proves the whole chain works end to end: this file is compiled
 * completely separately from the kernel (see tools/build_mwp.sh), never
 * linked against kernel.c in any way, then loaded off disk at runtime
 * by kernel/mwp.h's mwp_run() and jumped into -- and it still manages
 * to draw on screen and read the keyboard, using nothing but the fixed
 * syscall table both sides agreed on in advance (see mwp_api.h).
 *
 * Deliberately does nothing clever: fills the screen, draws a message,
 * waits for any key, then returns. No windows, no mouse, no icons --
 * Terminal.mwp (a later, separate piece of work) is what actually gives
 * loaded programs a place to live in the UI. This one file's whole job
 * is to be small and obviously correct enough that if something's wrong
 * with the loader, the bug is in the loader, not hiding in a
 * complicated demo.
 *
 * mwp_entry is the ONLY function this file defines, and mwp_link.ld
 * places it at the very first byte of the compiled output on purpose
 * (see that file's SECTIONS block) -- mwp_run() jumps to
 * MWP_LOAD_ADDR + entry_offset with entry_offset always 0, so whatever
 * lands at byte 0 IS the program's entry point, no symbol table lookup
 * involved. Renaming this function without also updating mwp_link.ld's
 * ".text.mwp_entry" section name would break that. */
void mwp_entry(void) {
    sys.fill_rect(0, 0, (int)sys.screen_w, (int)sys.screen_h, MWP_COL_BLUE);

    sys.draw_string(40, 40, "GREETER.MWP", MWP_COL_WHITE);
    sys.draw_string_ko(40, 60, "\xec\x95\x88\xeb\x85\x95, MiniWin!", MWP_COL_WHITE);
    /* "안녕, MiniWin!" in UTF-8 -- see kernel/font_ko.h's
     * ko_draw_mixed_string() for why a mixed Latin+Hangul string is
     * passed as one plain byte string rather than through any special
     * wide-char type: this kernel has no wchar_t, no libc, and
     * ko_draw_mixed_string() already knows how to tell a 3-byte UTF-8
     * Hangul sequence apart from a plain ASCII byte on its own. */
    sys.draw_string(40, 90, "This program was loaded from disk", MWP_COL_WHITE);
    sys.draw_string(40, 105, "at runtime -- not built into the kernel.", MWP_COL_WHITE);
    sys.draw_string(40, 130, "Press any key to return to Terminal...", MWP_COL_WHITE);

    /* Every draw_ and fill_rect/put_pixel call above only touched the
     * off-screen backbuffer (see mwp_api.h's present field) -- so
     * nothing drawn is actually visible on screen until this call. */
    sys.present();

    /* Block until a key comes in. key_poll() never blocks on its own
     * (see kernel/mwp.h's syscall table comment -- it's the same
     * never-blocks contract the kernel's own main loop relies on), so
     * "wait for a key" here just means "keep asking until it's not 0,"
     * exactly the same busy-poll pattern kernel.c's own main loop uses
     * for everything else. A real OS would put the CPU to sleep between
     * polls; this one doesn't have an idle/halt convention worth
     * introducing for a two-second demo program. */
    while (sys.key_poll() == 0) {
        /* spin */
    }
}
