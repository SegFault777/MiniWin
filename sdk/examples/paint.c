/* SPDX-License-Identifier: GPL-3.0-only */
/* paint.c -- the mouse API: hold the left button to draw, right button to erase, click a swatch to pick a colour,
 * and 'S' saves a quarter-size copy of the canvas (one colour index per byte, 100x70) to /PAINT.RAW with file_write.
 * Shows the error path too: when the disk has no room, err_text says why in the status line. ESC quits. */
#include "miniwin.h"

void mwp_entry(void) {
    if (!mw_has(2)) return;
    static const u32 pal[8] = { MW_BLACK, MW_RED, MW_GREEN, MW_BLUE, MW_YELLOW, MW_WHITE, MW_SHADOW, MW_ACCENT };
    const int CW = 400, CH = 280, CX = ((int)sys.screen_w - CW) / 2, CY = 50;
    u8 *px = sys.mem_alloc((u32)(CW * CH));              /* one colour index per canvas pixel: 112 KB from the kernel heap */
    if (!px) return;
    int color = 0, mx = 0, my = 0, mb = 0;
    char status[48]; status[0] = 0;
    mw_fill_screen(MW_FACE);
    for (int i = 0; i < CW * CH; i++) px[i] = 5;          /* index 5 = white */
    sys.window_frame(CX - 8, CY - 24, CW + 16, CH + 60, "PAINT.MWP", MWI_NOTEPAD);
    sys.fill_rect(CX, CY, CW, CH, MW_WHITE);              /* after the frame: the frame paints its whole body grey */
    for (;;) {
        sys.mouse_poll(&mx, &my, &mb);
        if (mb && mw_in_rect(mx, my, CX, CY, CW, CH)) {
            u32 c = (mb & 2) ? MW_WHITE : pal[color];
            int cx = mx - CX, cy = my - CY;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= CW || y >= CH) continue;
                px[y * CW + x] = (mb & 2) ? 5 : (u8)color; sys.put_pixel(CX + x, CY + y, c);
            }
        } else if (mb & 1) {
            for (int i = 0; i < 8; i++) if (mw_in_rect(mx, my, CX + i * 28, CY + CH + 6, 24, 18)) color = i;
        }
        for (int i = 0; i < 8; i++) { sys.fill_rect(CX + i * 28, CY + CH + 6, 24, 18, pal[i]); sys.draw_rect(CX + i * 28 - (i == color), CY + CH + 6 - (i == color), 24 + 2 * (i == color), 18 + 2 * (i == color), MW_BLACK); }
        mw_text(CX + 232, CY + CH + 8, status, MW_BLACK);
        sys.present();
        int k = sys.key_poll();
        if (k == MW_KEY_ESC) break;
        if (k == 's' || k == 'S') {
            u8 *small = sys.mem_alloc(100 * 70);
            if (small) for (int y = 0; y < 70; y++) for (int x = 0; x < 100; x++) small[y * 100 + x] = px[(y * 4) * CW + x * 4];
            int e = small ? sys.file_write("/PAINT.RAW", small, 100 * 70) : -2;
            sys.mem_free(small);
            int n = 0; status[0] = 0;
            if (e == 0) mw_append(status, &n, "saved /PAINT.RAW"); else mw_append(status, &n, sys.err_text(e));
            sys.fill_rect(CX + 232, CY + CH + 4, 160, 20, MW_FACE);
        }
    }
    sys.mem_free(px);
}
